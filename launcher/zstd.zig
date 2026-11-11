//! zstd decompression for the launcher, from Zig's standard library (the launcher's C code has no
//! decompressor of its own). Built as an object file and linked into the C programs.
const std = @import("std");

/// Decompress one zstd stream into dst. dst_cap must be the content size plus 128 KB (the decoder
/// wants room for a whole block ahead). Returns the number of bytes written, 0 on any error.
export fn nr_zstd_decompress(src: [*]const u8, src_len: usize, dst: [*]u8, dst_cap: usize) usize {
    var in: std.Io.Reader = .fixed(src[0..src_len]);
    var out: std.Io.Writer = .fixed(dst[0..dst_cap]);
    // The game's regulation.bin declares a 128 MB window.
    var stream: std.compress.zstd.Decompress = .init(&in, &.{}, .{ .window_len = 1 << 27 });
    return stream.reader.streamRemaining(&out) catch 0;
}
