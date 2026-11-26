#!/usr/bin/env python3
"""Real dlopen / audio FX v2 ABI smoke test, using the actual shared library."""
import ctypes as c
import json
import pathlib
import random
import sys
class API(c.Structure):
    _fields_ = [("version", c.c_uint32)] + [(k, c.c_void_p) for k in
        ("create", "destroy", "process", "set", "get", "midi")]
lib = c.CDLL(sys.argv[1])
lib.move_audio_fx_init_v2.restype = c.POINTER(API)
api = lib.move_audio_fx_init_v2(None).contents
assert api.version == 2 and not api.midi
create = c.CFUNCTYPE(c.c_void_p, c.c_char_p, c.c_char_p)(api.create)
destroy = c.CFUNCTYPE(None, c.c_void_p)(api.destroy)
process = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(c.c_int16), c.c_int)(api.process)
set_param = c.CFUNCTYPE(None, c.c_void_p, c.c_char_p, c.c_char_p)(api.set)
get = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_char_p, c.c_char_p, c.c_int)(api.get)
manifest = json.loads((pathlib.Path(__file__).resolve().parent.parent / "module.json").read_text())
p = create(b".", None)
assert p
def read(key, size=8192):
    out = c.create_string_buffer(size)
    n = get(p, key.encode(), out, size)
    assert n >= 0, key
    return out.value.decode()
def write(key, value):
    set_param(p, key.encode(), str(value).encode())
rng = random.Random(7)
def block(frames=128, amp=12000):
    return (c.c_int16 * (2 * frames))(*[rng.randrange(-amp, amp) for _ in range(2 * frames)])

# Metadata is exactly module.json's.
metadata = json.loads(read("chain_params"))
hierarchy = json.loads(read("ui_hierarchy"))
assert metadata == manifest["capabilities"]["chain_params"]
assert hierarchy == manifest["capabilities"]["ui_hierarchy"]
assert read("name") == "Dirt Delay"
for m in metadata:
    got = float(read(m["key"]))
    assert got == float(m["default"]), (m["key"], got)

# Mix 0: bit-exact passthrough. Silence stays silent.
write("mix", 0)
for _ in range(100): process(p, block(), 128)  # the 10 ms glide lands
for _ in range(20):
    buf = block(); ref = list(buf)
    process(p, buf, 128)
    assert list(buf) == ref
write("mix", 1)
fresh = create(b".", None)
zero = (c.c_int16 * 256)()
for _ in range(400): process(fresh, zero, 128)
assert not any(zero)
destroy(fresh)
# A tail rings out to exact digital silence (no endless denormal hiss).
for _ in range(2000):
    zero = (c.c_int16 * 256)()
    process(p, zero, 128)
assert not any(zero)
print("PASS: dlopen, audio FX v2 ABI, metadata, defaults, dry identity, silence")

# Enums take index or name and always report the index.
for key, opts in [("fx1", ["Off", "OD", "BC"]), ("fx2", ["Off", "OD", "BC"])]:
    for i, name in enumerate(opts):
        write(key, i); assert read(key) == str(i)
        write(key, 0); write(key, name.lower() if i % 2 else name); assert read(key) == str(i)
    for bad in ["1.5", "Offx", "", "-", "nan", "BCD"]:
        before = read(key); write(key, bad); assert read(key) == before, (key, bad)
for gone in ["boost", "cutoff", "res", "match"]:  # removed in v0.3
    small = c.create_string_buffer(64)
    assert get(p, gone.encode(), small, 64) == -1
write("feedback", 3); assert read("feedback") == "0.9"
write("time", -4); assert read("time") == "0"
write("drive", 0); assert read("drive") == "0.3"
write("gain", 30); assert read("gain") == "24"
write("gain", -3); assert read("gain") == "0"
for key in ["mix", "time", "gain"]:
    before = read(key)
    for bad in ["nan", "inf", "-inf", "garbage", "0.1junk", ""]:
        write(key, bad); assert read(key) == before

# State: round trip, pretty-printed accepted, everything malformed refused.
values = {"mix": 0.7, "gain": 9.5, "drive": 0.65, "time": 80, "feedback": 0.55,
          "crush": 0.25, "fx1": 2, "fx2": 1}
for k, v in values.items(): write(k, v)
state = read("state")
decoded = json.loads(state)
assert decoded.pop("version") == 3 and list(decoded.keys()) == list(values.keys())
assert all(abs(decoded[k] - v) < 1e-6 for k, v in values.items())
other = create(b".", None); assert other
out = c.create_string_buffer(1024)
set_param(other, b"state", json.dumps(json.loads(state), indent=2).encode())
assert get(other, b"state", out, 1024) > 0 and out.value.decode() == state
destroy(other)
for invalid in ["{}", "{", "", state[:-1], state + "junk",
                state.replace('"version":3', '"version":4'),
                state.replace('"version":3,', ''),
                state.replace('"version":3', '"version":3,"version":3'),
                state.replace(',"gain":9.5', ''),
                state.replace('"version":3', '"version":2'),       # v2 has no gain
                state.replace('}', ',"boost":0}'),                  # v3 carries no obsolete key
                state.replace('"mix":', '"unknown":'),
                state.replace('"mix":', '"crush":'),
                state.replace('"fx1":2', '"fx1":3'),
                state.replace('"fx1":2', '"fx1":1.5'),
                state.replace('"drive":0.649999976', '"drive":0.2'),  # below the v0.3 floor
                state.replace('"gain":9.5', '"gain":25'),
                state.replace('"time":80', '"time":251')]:
    write("state", invalid)
    assert read("state") == state, invalid
# Older builds' states still load: v0.1 (version 1) and v0.2 (version 2).
v01 = ('{"version":1,"mix":0.3,"crush":0.5,"drive":0.1,"time":90,"feedback":0.4,'
       '"cutoff":1,"res":0,"boost":1,"fx1":1,"fx2":2}')
v02 = v01.replace('"version":1', '"version":2').replace('"fx2":2}', '"fx2":2,"match":0}')
for old in [v01, v02]:
    h = create(b".", None); assert h
    set_param(h, b"gain", b"6")
    set_param(h, b"state", old.encode())
    got = c.create_string_buffer(1024)
    assert get(h, b"state", got, 1024) > 0
    d = json.loads(got.value)
    assert d["version"] == 3 and abs(d["mix"] - 0.3) < 1e-6 and d["time"] == 90
    assert abs(d["drive"] - 0.3) < 1e-6, "an old Drive below 0.3 is raised to the floor"
    assert d["gain"] == 0 and d["fx1"] == 1 and d["fx2"] == 2
    for bad in [old.replace(',"boost":1', ''), old.replace('"fx1":1', '"fx1":1,"gain":3')]:
        set_param(h, b"state", b'{"version":3,"mix":0.5,"gain":1,"drive":0.5,"time":10,"feedback":0,"crush":0,"fx1":0,"fx2":0}')
        before = (c.create_string_buffer(1024)); get(h, b"state", before, 1024)
        set_param(h, b"state", bad.encode())
        after = c.create_string_buffer(1024); get(h, b"state", after, 1024)
        assert before.value == after.value, bad
    destroy(h)
for _ in range(5000):
    bad = "".join(rng.choice('{}:,."0123456789e+- abcdefmixtv') for _ in range(rng.randrange(80)))
    write("state", bad)
    assert read("state") == state
for key in ["state", "chain_params", "ui_hierarchy", "mix", "name"]:
    small = c.create_string_buffer(2)
    assert get(p, key.encode(), small, 2) == -1 and small.value == b""

# Every switch combination at full tilt: output stays in range and non-silent.
for a in range(3):
    for b in range(3):
        write("fx1", a); write("fx2", b); write("feedback", 0.9); write("gain", 24)
        energy = 0
        for _ in range(200):
            buf = block(amp=32767)
            process(p, buf, 128)
            energy += sum(x * x for x in buf)
        assert energy > 0, (a, b)

# Null instance / pointer and odd frame counts leave the buffer alone or in bounds.
for frames in [1, 17, 128, 257]:
    guarded = (c.c_int16 * (2 * frames + 2))()
    guarded[0] = 1234; guarded[-1] = 5678
    process(p, c.cast(c.byref(guarded, 2), c.POINTER(c.c_int16)), frames)
    assert guarded[0] == 1234 and guarded[-1] == 5678
process(None, zero, 128); process(p, None, 128)
set_param(None, b"mix", b"1"); set_param(p, None, b"1"); set_param(p, b"mix", None)
assert get(None, b"mix", out, 1024) == -1

# Pool: sixteen instances, then refusal, then reuse.
handles = [p] + [create(b".", None) for _ in range(15)]
assert all(handles) and not create(b".", None)
for h in handles: destroy(h)
p = create(b".", None); assert p
assert read("mix") == "0.5" and read("fx1") == "0" and read("gain") == "0"
destroy(p)
pathlib.Path(sys.argv[1]).with_name("runtime-contract.json").write_text(
    json.dumps({"hierarchy": hierarchy, "chainParams": metadata}))
print("PASS: enums, clamping, state round-trip incl. v0.1/v0.2 states, malformed/fuzz input, switch matrix, bounds, pool")
