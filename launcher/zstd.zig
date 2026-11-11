//! zstd decompression and zlib compression for the launcher, from Zig's standard library (the
//! launcher's C code has neither). Built as an object file and linked into the C programs.
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

/// Compress src as a zlib stream into dst. Returns the number of bytes written, 0 on any error
/// (dst too small included).
export fn nr_zlib_compress(src: [*]const u8, src_len: usize, dst: [*]u8, dst_cap: usize) usize {
    const flate = std.compress.flate;
    var out: std.Io.Writer = .fixed(dst[0..dst_cap]);
    var window: [flate.max_window_len]u8 = undefined;
    var stream = flate.Compress.init(&out, &window, .zlib, .default) catch return 0;
    stream.writer.writeAll(src[0..src_len]) catch return 0;
    stream.finish() catch return 0;
    return out.end;
}
