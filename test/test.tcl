# See the file LICENSE for redistribution information.
#
# Copyright (c) 1996-2004
#	Sleepycat Software.  All rights reserved.

source ./include.tcl

# Load DBSQL's TCL API.
load $tcllib

if { [file exists $testdir] != 1 } {
	file mkdir $testdir
}

global __debug_print
global __debug_on
global __debug_test

#
# Test if utilities work to figure out the path.  Most systems
# use ., but QNX has a problem with execvp of shell scripts which
# causes it to break.
#
set stat [catch {exec ./dbsql --help} ret]
if { [string first "exec format error" $ret] != -1 } {
	set util_path ./.libs
} else {
	set util_path .
}
set __debug_print 0

# Error stream that (should!) always go to the console, even if we're
# redirecting to ALL.OUT.
set consoleerr stderr

set dict $test_path/wordlist
set alphabet "abcdefghijklmnopqrstuvwxyz"
set datastr "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"

# Random number seed.
global rand_init
set rand_init 12082004

# Open handles global check
set ohandles {}

# Normally, we're not running an all-tests-in-one-env run.  This matters
# for error stream/error prefix settings in berkdbsql_open.
global is_envmethod
set is_envmethod 0

# The variable one_test allows us to run all the permutations
# of a test with run_all or run_std.
global one_test
if { [info exists one_test] != 1 } {
	set one_test "ALL"
}

# This is where the test numbering and parameters now live.
source $test_path/testparams.tcl

# Set up any OS-specific values
global tcl_platform
set is_windows_test [is_substr $tcl_platform(os) "Win"]
set is_hp_test [is_substr $tcl_platform(os) "HP-UX"]
set is_je_test 0
set is_qnx_test [is_substr $tcl_platform(os) "QNX"]
set upgrade_be [big_endian]

global EXE BAT
if { $is_windows_test == 1 } {
	set EXE ".exe"
	set BAT ".bat"
} else {
	set EXE ""
	set BAT ""
}

# From here on out, test.tcl contains the procs that are used to
# run all or part of the test suite.

proc run_std { { testname ALL } args } {
	global test_names
	global one_test
	source ./include.tcl

	set one_test $testname
	if { $one_test != "ALL" } {
		# Source testparams again to adjust test_names.
		source $test_path/testparams.tcl
	}

	set exflgs [eval extractflags $args]
	set args [lindex $exflgs 0]
	set flags [lindex $exflgs 1]

	set display 1
	set run 1
	set std_only 1
	set rflags {--}
	foreach f $flags {
		switch $f {
			A {
				set std_only 0
			}
			n {
				set display 1
				set run 0
				set rflags [linsert $rflags 0 "-n"]
			}
		}
	}

	if { $std_only == 1 } {
		fileremove -f ALL.OUT

		set o [open ALL.OUT a]
		if { $run == 1 } {
			puts -nonewline "Test suite run started at: "
			puts [clock format [clock seconds] -format "%H:%M %D"]
			puts [berkdbsql version -string]

			puts -nonewline $o "Test suite run started at: "
			puts $o [clock format [clock seconds] -format "%H:%M %D"]
			puts $o [berkdbsql version -string]
		}
		close $o
	}

	set test_list {
#	{"environment"		"env"}
	}

	foreach pair $test_list {
		set msg [lindex $pair 0]
		set cmd [lindex $pair 1]
		puts "Running $msg tests"
		if [catch {exec $tclsh_path << \
		    "global one_test; set one_test $one_test; \
		    source $test_path/test.tcl; r $rflags $cmd" \
		    >>& ALL.OUT } res] {
			set o [open ALL.OUT a]
			puts $o "FAIL: $cmd test: $res"
			close $o
		}
	}

	# If not actually running, no need to check for failure.
	# If running in the context of the larger 'run_all' we don't
	# check for failure here either.
	if { $run == 0 || $std_only == 0 } {
		return
	}

	set failed [check_output ALL.OUT]

	set o [open ALL.OUT a]
	if { $failed == 0 } {
		puts "Regression Tests Succeeded"
		puts $o "Regression Tests Succeeded"
	} else {
		puts "Regression Tests Failed"
		puts "Check UNEXPECTED OUTPUT lines."
		puts "Review ALL.OUT.x for details."
		puts $o "Regression Tests Failed"
	}

	puts -nonewline "Test suite run completed at: "
	puts [clock format [clock seconds] -format "%H:%M %D"]
	puts -nonewline $o "Test suite run completed at: "
	puts $o [clock format [clock seconds] -format "%H:%M %D"]
	close $o
}

proc check_output { file } {
	# These are all the acceptable patterns.
	# TODO update these patterns
	set pattern {(?x)
		^[:space:]*$|
		.*?wrap\.tcl.*|
		.*?dbscript\.tcl.*|
		.*?ddscript\.tcl.*|
		.*?mpoolscript\.tcl.*|
		.*?mutexscript\.tcl.*|
		^\d\d:\d\d:\d\d\s\(\d\d:\d\d:\d\d\)$|
		^\d\d:\d\d:\d\d\s\(\d\d:\d\d:\d\d\)\sCrashing$|
		^\d\d:\d\d:\d\d\s\(\d\d:\d\d:\d\d\)\s[p|P]rocesses\srunning:.*|
		^\d\d:\d\d:\d\d\s\(\d\d:\d\d:\d\d\)\s5\sprocesses\srunning.*|
		^\d:\sPut\s\d*\sstrings\srandom\soffsets.*|
		^100.*|
		^eval\s.*|
		^exec\s.*|
		^jointest.*$|
		^r\sarchive\s*|
		^r\sdbm\s*|
		^r\shsearch\s*|
		^r\sndbm\s*|
		^r\srpc\s*|
		^run_recd:\s.*|
		^run_reptest:\s.*|
		^run_rpcmethod:\s.*|
		^run_secenv:\s.*|
		^All\sprocesses\shave\sexited.$|
		^Beginning\scycle\s\d$|
		^Byteorder:.*|
		^Child\sruns\scomplete\.\s\sParent\smodifies\sdata\.$|
		^Deadlock\sdetector:\s\d*\sCheckpoint\sdaemon\s\d*$|
		^Ending\srecord.*|
		^Environment\s.*?specified;\s\sskipping\.$|
		^Executing\srecord\s.*|
		^Join\stest:\.*|
		^Method:\s.*|
		^Repl:\stest\d\d\d:.*|
		^Repl:\ssdb\d\d\d:.*|
		^Script\swatcher\sprocess\s.*|
		^Secondary\sindex\sjoin\s.*|
		^Sleepycat\sSoftware:\sBerkeley\sDBSQL\s.*|
		^Test\ssuite\srun\s.*|
		^Unlinking\slog:\serror\smessage\sOK$|
		^Verifying\s.*|
		^\t*\.\.\.dbc->get.*$|
		^\t*\.\.\.dbc->put.*$|
		^\t*\.\.\.key\s\d*$|
		^\t*\.\.\.Skipping\sdbc.*|
		^\t*and\s\d*\sduplicate\sduplicates\.$|
		^\t*About\sto\srun\srecovery\s.*complete$|
		^\t*Archive[:\.].*|
		^\t*Building\s.*|
		^\t*closing\ssecondaries\.$|
		^\t*Command\sexecuted\sand\s.*$|
		^\t*DBM.*|
		^\t*[d|D]ead[0-9][0-9][0-9].*|
		^\t*Dump\/load\sof.*|
		^\t*[e|E]nv[0-9][0-9][0-9].*|
		^\t*Executing\scommand$|
		^\t*Executing\stxn_.*|
		^\t*File\srecd005\.\d\.db\sexecuted\sand\saborted\.$|
		^\t*File\srecd005\.\d\.db\sexecuted\sand\scommitted\.$|
		^\t*[f|F]op[0-9][0-9][0-9].*|
		^\t*HSEARCH.*|
		^\t*Initial\sCheckpoint$|
		^\t*Iteration\s\d*:\sCheckpointing\.$|
		^\t*Joining:\s.*|
		^\t*Kid[1|2]\sabort\.\.\.complete$|
		^\t*Kid[1|2]\scommit\.\.\.complete$|
		^\t*[l|L]ock[0-9][0-9][0-9].*|
		^\t*[l|L]og[0-9][0-9][0-9].*|
		^\t*[m|M]emp[0-9][0-9][0-9].*|
		^\t*[m|M]utex[0-9][0-9][0-9].*|
		^\t*NDBM.*|
		^\t*opening\ssecondaries\.$|
		^\t*op_recover_rec:\sRunning\srecovery.*|
		^\t*[r|R]ecd[0-9][0-9][0-9].*|
		^\t*[r|R]ep[0-9][0-9][0-9].*|
		^\t*[r|R]ep_test.*|
		^\t*[r|R]pc[0-9][0-9][0-9].*|
		^\t*[r|R]src[0-9][0-9][0-9].*|
		^\t*Run_rpcmethod.*|
		^\t*Running\srecovery\son\s.*|
		^\t*[s|S]ec[0-9][0-9][0-9].*|
		^\t*[s|S]i[0-9][0-9][0-9].*|
		^\t*[s|S]ijoin.*|
		^\t*sdb[0-9][0-9][0-9].*|
		^\t*Skipping\s.*|
		^\t*Subdb[0-9][0-9][0-9].*|
		^\t*Subdbtest[0-9][0-9][0-9].*|
		^\t*Syncing$|
		^\t*[t|T]est[0-9][0-9][0-9].*|
		^\t*[t|T]xn[0-9][0-9][0-9].*|
		^\t*Txnscript.*|
		^\t*Using\s.*?\senvironment\.$|
		^\t*Verification\sof.*|
		^\t*with\stransactions$}

	set failed 0
	set f [open $file r]
	while { [gets $f line] >= 0 } {
		if { [regexp $pattern $line] == 0 } {
			puts -nonewline "UNEXPECTED OUTPUT: "
			puts $line
			set failed 1
		}
	}
	close $f
	return $failed
}

proc r { args } {
	global test_names
	global rand_init
	global one_test

	source ./include.tcl

	set exflgs [eval extractflags $args]
	set args [lindex $exflgs 0]
	set flags [lindex $exflgs 1]

	set display 1
	set run 1
	set saveflags "--"
	foreach f $flags {
		switch $f {
			n {
				set display 1
				set run 0
				set saveflags "-n $saveflags"
			}
		}
	}

	if {[catch {
		set sub [ lindex $args 0 ]
		switch $sub {
			shelltest {
				if { $one_test == "ALL" } {
					if { $display } { puts "r $sub" }
					if { $run } {
						check_handles
						$sub
					}
				}
			}
			default {
				error \
				   "FAIL:[timestamp] r: $args: unknown command"
			}
		}
		flush stdout
		flush stderr
	} res] != 0} {
		global errorInfo;

		set fnl [string first "\n" $errorInfo]
		set theError [string range $errorInfo 0 [expr $fnl - 1]]
		if {[string first FAIL $errorInfo] == -1} {
			error "FAIL:[timestamp] r: $args: $theError"
		} else {
			error $theError;
		}
	}
}

proc run_all { { testname ALL } args } {
	global test_names
	global one_test
	source ./include.tcl

	fileremove -f ALL.OUT

	set one_test $testname
	if { $one_test != "ALL" } {
		# Source testparams again to adjust test_names.
		source $test_path/testparams.tcl
	}

	set exflgs [eval extractflags $args]
	set flags [lindex $exflgs 1]
	set display 1
	set run 1
	set parallel 0
	set nparalleltests 0
	set rflags {--}
	foreach f $flags {
		switch $f {
			n {
				set display 1
				set run 0
				set rflags [linsert $rflags 0 "-n"]
			}
		}
	}

	set o [open ALL.OUT a]
	if { $run == 1 } {
		puts -nonewline "Test suite run started at: "
		puts [clock format [clock seconds] -format "%H:%M %D"]
		puts [berkdb version -string]

		puts -nonewline $o "Test suite run started at: "
		puts $o [clock format [clock seconds] -format "%H:%M %D"]
		puts $o [berkdbsql version -string]
	}
	close $o
	#
	# First run standard tests.  Send in a -A to let run_std know
	# that it is part of the "run_all" run, so that it doesn't
	# print out start/end times.
	#
	lappend args -A
	eval {run_std} $one_test $args

	set args [lindex $exflgs 0]
	set save_args $args

	foreach pair $test_list {
		set msg [lindex $pair 0]
		set cmd [lindex $pair 1]
		puts "Running $msg tests"
		if [catch {exec $tclsh_path << \
		    "global one_test; set one_test $one_test; \
		    source $test_path/test.tcl; \
		    r $rflags $cmd $args" >>& ALL.OUT } res] {
			set o [open ALL.OUT a]
			puts $o "FAIL: $cmd test: $res"
			close $o
		}
	}

	# If not actually running, no need to check for failure.
	if { $run == 0 } {
		return
	}

	set failed 0
	set o [open ALL.OUT r]
	while { [gets $o line] >= 0 } {
		if { [regexp {^FAIL} $line] != 0 } {
			set failed 1
		}
	}
	close $o
	set o [open ALL.OUT a]
	if { $failed == 0 } {
		puts "Regression Tests Succeeded"
		puts $o "Regression Tests Succeeded"
	} else {
		puts "Regression Tests Failed; see ALL.OUT for log"
		puts $o "Regression Tests Failed"
	}

	puts -nonewline "Test suite run completed at: "
	puts [clock format [clock seconds] -format "%H:%M %D"]
	puts -nonewline $o "Test suite run completed at: "
	puts $o [clock format [clock seconds] -format "%H:%M %D"]
	close $o
}

proc run { proc_suffix method {start 1} {stop 999} } {
	global test_names

	switch -exact -- $proc_suffix {
		default {
			puts "$proc_suffix is not set up with to be used with run"
		}
	}
}
