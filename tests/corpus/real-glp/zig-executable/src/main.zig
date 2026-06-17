extern "c" fn puts([*:0]const u8) c_int;

export fn main() c_int {
    _ = puts("zig-exe-ok");
    return 0;
}
