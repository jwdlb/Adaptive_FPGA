`timescale 1ns/1ps
module fixed_point_pkg_tb;
  import fixed_point_pkg::*;

  initial begin
    assert (rounded_divide_i64(64'sd1, 64'sd2) == 64'sd1);
    assert (rounded_divide_i64(-64'sd1, 64'sd2) == -64'sd1);
    assert (rounded_divide_i64(64'sd1, 64'sd3) == 64'sd0);
    assert (multiply_q16(32'sd1, 32'sd32768) == 32'sd1);
    assert (ratio_q16(64'sd1, 64'sd3) == 32'sd21845);
    assert (saturate_i64_to_i32(64'sd3000000000) == 32'sh7fff_ffff);
    assert (saturate_i64_to_i32(-64'sd3000000000) == 32'sh8000_0000);
    $display("fixed_point_pkg tests passed.");
    $finish;
  end
endmodule
