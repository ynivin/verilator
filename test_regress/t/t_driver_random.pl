#!/usr/bin/perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2003 by Wilson Snyder. This program is free software; you can
# redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.

use IO::File;
use Time::HiRes;

scenarios(dist => 1);

if (!$ENV{VERILATOR_TEST_RANDOM_FAILURE}) {
    skip("Test is for harness checking only, setenv VERILATOR_TEST_RANDOM_FAILURE=1 ");
} else {
    # Randomly fail to test driver.pl
    my ($ign, $t) = Time::HiRes::gettimeofday();
    if ($t % 2) {
        error("random failure ".$t);
    }
    else {
        ok(1);
    }
}
1;
