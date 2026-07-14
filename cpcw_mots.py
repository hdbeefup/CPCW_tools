"""
cpcw_mots.py -- reader for the SRM ``MOTS`` chunk (skeletal / node animation).

Reverse-engineered here (July 2026). CPCW ``.srm`` files optionally carry a
top-level ``MOTS`` chunk after ``PMOD`` that stores node animation as keyframed
transforms -- the ambient/scripted motions the engine plays: construction rise,
machinery loops (lighthouse beam, radar dish, oil derrick), open/close, and
destruction. 95 of the 2087 shipped models have one.

This is DISTINCT from the runtime "settle" of animated bones (tank road wheels /
suspension), which is terrain-contact IK computed at runtime and is NOT stored
in the file -- no amount of MOTS decoding recovers it.

Container layout (all little-endian; offsets are chunk-body-relative)
--------------------------------------------------------------------
MOTS body:
    'v004'                                   version
    repeated MOTI chunks (tag + u32 size)

MOTI (one *motion*, e.g. 'object_meshloopplaying_idx1'):
    u16 name_len, name bytes
    i32 target_obj                           (object/LOD index, 0 = base)
    f32 weight                               (1.0 in every shipped file)
    i32 unk0                                 (0)
    i32 node_count                           (== the model's node count)
    i32 node_channel[node_count]             per-node channel index, -1 = static
    ANIM chunk

ANIM (tag + u32 size):
    'v002'                                   version
    f32 duration                             seconds
    i32 unk[3]   (0, 0, 1)
    i32 channel_count
    i32 header_size (28)  then pad to 32
    channel record[channel_count]            (68 bytes each)
    keyframe pool                            (68-byte records, packed per channel)

channel record (68 bytes):
    f32 base[10]        summary/rest floats (first key's value(s) echoed here)
    i32 key_count
    i32 unk
    i32 key_offset      byte offset (ANIM-body relative) of this channel's keys
    i32 pad[4]

keyframe record (68 bytes) -- fields VERIFIED to carry the animated value;
the time/interpolation semantics (marked provisional) still want a Ghidra pass:
    i32 link_in         (chains to previous key's link_out)
    i32 [1..2]          (0)
    i32 a               (4)      provisional: data type/count
    i32 b               (2)      provisional
    i32 [5]             (0)
    f32 t_or_dur        (== ANIM.duration in every key seen -- NOT a per-key time)
    f32 value0          the animated scalar (e.g. Y-rotation angle; -6.283 = -2pi)
    f32 value1          (== value0 for interpolated keys)
    f32 [9..12]         (0)
    i32 c               (201)    provisional
    i32 d               (2)      provisional
    i32 pool_offset     (cumulative; +~402 per key)
    i32 link_out        (chains to next key's link_in)

Because ``t_or_dur`` is constant, per-key timing is currently taken as uniform
over ``duration`` (keys evenly spaced). Confirm against the engine before
treating playback timing as ground-truth.

Ghidra cross-check (CPCWu.exe; see N:/gamePAKdata/re/MOTS_RESULT.md)
-------------------------------------------------------------------
The loader ``FUN_004d4e40`` (MOTS/MOTI) + parser ``FUN_004ce310`` (ANIM 'v002')
CONFIRM this layout from the engine side: the fixup loop walks channels and
keyframes both at stride 0x11 ints = **68 bytes**, and treats the per-key field
at ``key+0x3c`` (our ``pool_offset``) as a relocated pointer. The ANIM block's
``+0xc`` is an interpolation-TYPE enum (0..3) that selects one of four
storage->engine **coordinate converters** (matrix / vec3 / quat / swizzled-with-
sign-flip), i.e. which transform component a channel drives. STILL OPEN in the
disassembly: the standalone per-frame evaluator (time->key + blend) that writes
the node's ``ActTransform@0x8`` / ``ActLocalMatrix@0x38`` -- so the time &
interpolation fields above remain provisional until it is read.
"""

import struct
import sys


def _u16(d, p): return struct.unpack_from('<H', d, p)[0]
def _i32(d, p): return struct.unpack_from('<i', d, p)[0]
def _u32(d, p): return struct.unpack_from('<I', d, p)[0]
def _f32(d, p): return struct.unpack_from('<f', d, p)[0]


class Keyframe:
    __slots__ = ('index', 'value', 'value1', 't_field', 'link_in', 'link_out',
                 'pool_offset', 'raw')

    def __repr__(self):
        return f"Key(v={self.value:+.4f})"


class Channel:
    __slots__ = ('base', 'key_count', 'key_offset', 'keys')

    def __repr__(self):
        return f"Channel(keys={self.key_count})"


class Motion:
    __slots__ = ('name', 'target_obj', 'weight', 'node_count',
                 'node_channel', 'duration', 'channels')

    def animated_nodes(self):
        """dict: node_index -> Channel (only nodes with channel != -1)."""
        out = {}
        for ni, ch in enumerate(self.node_channel):
            if ch != -1 and 0 <= ch < len(self.channels):
                out[ni] = self.channels[ch]
        return out

    def __repr__(self):
        return (f"Motion('{self.name}' dur={self.duration:.3f} "
                f"chans={len(self.channels)})")


def _get_mots_body(data):
    if data[:4] != b'MAIN':
        raise ValueError("not an SRM file")
    pos = 16 + _u32(data, 12)          # skip MAIN header + THMB
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        size = _u32(data, pos + 4)
        if tag == b'MOTS':
            return data[pos + 8:pos + 8 + size]
        pos += 8 + size
    return None


def _parse_anim(body, r, aend):
    """Parse an ANIM sub-chunk body starting at r (points at 'v002')."""
    assert body[r:r + 4] == b'v002', body[r:r + 4]
    r += 4
    duration = _f32(body, r)
    channel_count = _i32(body, r + 16)
    rec = r + 32                        # channel records start here
    channels = []
    for _ in range(channel_count):
        c = Channel()
        c.base = [_f32(body, rec + i * 4) for i in range(10)]
        c.key_count = _i32(body, rec + 40)
        c.key_offset = _i32(body, rec + 48)
        c.keys = []
        channels.append(c)
        rec += 68
    # keyframes: each channel's key_offset is ANIM-body(after v002)-relative
    kbase = r  # values are relative to the byte right after 'v002'
    for c in channels:
        o = kbase + c.key_offset
        for _ in range(c.key_count):
            if o + 68 > aend:
                break
            k = Keyframe()
            k.link_in = _i32(body, o + 0)
            k.t_field = _f32(body, o + 24)
            k.value = _f32(body, o + 28)
            k.value1 = _f32(body, o + 32)
            k.pool_offset = _i32(body, o + 60)
            k.link_out = _i32(body, o + 64)
            k.raw = body[o:o + 68]
            c.keys.append(k)
            o += 68
    return duration, channels


def parse_mots(path):
    """Parse the MOTS chunk of an SRM file. Returns list[Motion] (maybe empty)."""
    with open(path, 'rb') as f:
        data = f.read()
    body = _get_mots_body(data)
    if body is None:
        return []
    assert body[:4] == b'v004', body[:4]
    motions = []
    p = 4
    while p + 8 <= len(body):
        tag = body[p:p + 4]
        size = _u32(body, p + 4)
        end = p + 8 + size
        if tag != b'MOTI':
            p = end
            continue
        q = p + 8
        nlen = _u16(body, q); q += 2
        m = Motion()
        m.name = body[q:q + nlen].decode('latin1'); q += nlen
        m.target_obj = _i32(body, q)
        m.weight = _f32(body, q + 4)
        m.node_count = _i32(body, q + 12)
        q += 16
        m.node_channel = [_i32(body, q + i * 4) for i in range(m.node_count)]
        q += m.node_count * 4
        assert body[q:q + 4] == b'ANIM', body[q:q + 4]
        asize = _u32(body, q + 4)
        m.duration, m.channels = _parse_anim(body, q + 8, q + 8 + asize)
        motions.append(m)
        p = end
    return motions


# ---------------------------------------------------------------------------
# PROVISIONAL playback (not engine-proven -- see the "still open" note above).
#
# Empirical model, from the shipped data: a channel's ``key_count`` records are
# NOT time samples -- they are the up-to-6 per-DOF component tracks of the node's
# local transform, in order (tx, ty, tz, rx, ry, rz). Five sit static at their
# ``base[10]`` rest value; the animated one carries the motion, and its span
# ``value1 - value0`` is the total change over ``duration``. For rotation tracks
# (dof 3/4/5) that span is an angle in radians about the node's local x/y/z --
# clean multiples of 2*pi (rotor -20*pi = 10 turns, tail rotor +40*pi, lamp -2*pi).
#
# Provisional choices (flagged; revisit once the engine evaluator is read):
#   * per-key time is uniform over ``duration`` (linear, constant speed);
#   * only the single dominant track per node is played (enough for spins/lifts);
#   * the dof index gives the NODE-LOCAL rotation axis (rx/ry/rz). To get a world
#     axis you must compose it with the node's rest orientation AND the importer's
#     handedness swap -- applying the dof axis to a Blender object's local euler
#     naively makes rotors tumble. blendertools/mots_play.py sidesteps this with a
#     robust geometric shortcut: a rotor/dish is thinnest along its shaft, so it
#     spins about the group's minimum-extent world axis through the centroid.
# NOTE: reliable when a channel carries all 6 DOF tracks (key_count==6, tracks in
# order tx,ty,tz,rx,ry,rz); channels with fewer tracks don't pin the dof by index.
# ---------------------------------------------------------------------------

_DOF_NAME = ['tx', 'ty', 'tz', 'rx', 'ry', 'rz']


class NodeSpin:
    """One node's dominant animated DOF over a motion (provisional)."""
    __slots__ = ('node_idx', 'dof', 'v0', 'v1', 'duration')

    @property
    def is_rotation(self):
        return self.dof >= 3

    @property
    def axis(self):
        """local axis index for a rotation dof (0=x,1=y,2=z), else None."""
        return (self.dof - 3) if self.dof >= 3 else None

    @property
    def span(self):
        return self.v1 - self.v0

    def sample(self, t, loop=True):
        """Value of this DOF at time t (seconds), linear over duration."""
        d = self.duration
        if d <= 0:
            return self.v1
        f = (t % d) / d if loop else max(0.0, min(1.0, t / d))
        return self.v0 + (self.v1 - self.v0) * f

    def __repr__(self):
        return ("NodeSpin(node=%d %s span=%+.3f dur=%.3f)"
                % (self.node_idx, _DOF_NAME[self.dof], self.span, self.duration))


def node_spins(motion, min_span=1e-3):
    """Provisional: dominant animated DOF per node. Returns list[NodeSpin].

    Picks, per animated node, the component track with the largest |value1-value0|
    (the moving DOF), skipping nodes whose largest span is below ``min_span``.
    """
    out = []
    for ni, ch in motion.animated_nodes().items():
        if not ch.keys:
            continue
        best_i, best_span = -1, 0.0
        for i, k in enumerate(ch.keys[:6]):
            s = abs(k.value1 - k.value)
            if s > best_span:
                best_span, best_i = s, i
        if best_i < 0 or best_span < min_span:
            continue
        ns = NodeSpin()
        ns.node_idx = ni
        ns.dof = best_i
        ns.v0 = ch.keys[best_i].value
        ns.v1 = ch.keys[best_i].value1
        ns.duration = motion.duration
        out.append(ns)
    return out


def _main(argv):
    if not argv:
        print("usage: cpcw_mots.py <model.srm> [...]")
        return 1
    for path in argv:
        motions = parse_mots(path)
        print(f"\n{path}: {len(motions)} motion(s)")
        for m in motions:
            an = m.animated_nodes()
            print(f"  {m}  target={m.target_obj} animated_nodes={list(an)}")
            for ni, ch in an.items():
                vals = ', '.join(f"{k.value:+.3f}" for k in ch.keys)
                print(f"    node[{ni}] ch keys[{ch.key_count}] values=[{vals}]")
            for ns in node_spins(m):
                turns = ns.span / (2 * 3.14159265) if ns.is_rotation else None
                extra = (" (%.2f turns about local %s)" % (turns, "xyz"[ns.axis])
                         if ns.is_rotation else " (translation)")
                print(f"    SPIN {ns}{extra}")
    return 0


if __name__ == '__main__':
    sys.exit(_main(sys.argv[1:]))
