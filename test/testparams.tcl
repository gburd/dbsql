
global one_test

set subs {test}

set test_names(test)	[list test001 ]

# Source all the tests, whether we're running one or many.
foreach sub $subs {
	foreach test $test_names($sub) {
		source $test_path/$test.tcl
	}
}

# Reset test_names if we're running only one test.
if { $one_test != "ALL" } {
	foreach sub $subs {
		set test_names($sub) ""
	}
	set type [string trim $one_test 0123456789]
	set test_names($type) [list $one_test]
}

source $test_path/testutils.tcl

#set parms(test001) {10000 0 0 "001"}

# Shell script tests.  Each list entry is a {directory filename} pair,
# invoked with "/bin/sh filename".
set shelltest_list {
#	{ scr001	chk.code }
#	{ scr002	chk.def }
	{ scr003	chk.define }

	{ scr005	chk.nl }
	{ scr006	chk.offt }
	{ scr007	chk.proto }
#	{ scr008	chk.pubdef }
#	{ scr009	chk.srcfiles }
	{ scr010	chk.str }
	{ scr011	chk.tags }

#	{ scr013	chk.stats }
	{ scr014	chk.err }



	{ scr018	chk.comma }
	{ scr019	chk.include }
	{ scr020	chk.inc }
#	{ scr021	chk.flags }
#	{ scr022	chk.rr }



	{ scr026	chk.method }



	{ scr030	chk.build }
	{ scr050	chk.sqlite }
}
