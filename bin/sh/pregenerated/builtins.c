/*
 * This file was generated by the mkbuiltins program.
 */

#include <stdlib.h>
#include "shell.h"
#include "builtins.h"

int (*const builtinfunc[])(int, char **) = {
	bltincmd,
	aliascmd,
	bgcmd,
	bindcmd,
	breakcmd,
	cdcmd,
	commandcmd,
	dotcmd,
	echocmd,
	evalcmd,
	execcmd,
	exitcmd,
	letcmd,
	exportcmd,
	falsecmd,
	fgcmd,
	getoptscmd,
	hashcmd,
	histcmd,
	jobidcmd,
	jobscmd,
	killcmd,
	localcmd,
	printfcmd,
	pwdcmd,
	readcmd,
	returncmd,
	setcmd,
	setvarcmd,
	shiftcmd,
	testcmd,
	timescmd,
	trapcmd,
	truecmd,
	typecmd,
	ulimitcmd,
	umaskcmd,
	unaliascmd,
	unsetcmd,
	waitcmd,
	wordexpcmd,
};

const unsigned char builtincmd[] = {
	"\007\000builtin"
	"\005\001alias"
	"\002\002bg"
	"\004\003bind"
	"\005\204break"
	"\010\204continue"
	"\002\005cd"
	"\005\005chdir"
	"\007\006command"
	"\001\207."
	"\004\010echo"
	"\004\211eval"
	"\004\212exec"
	"\004\213exit"
	"\003\014let"
	"\006\215export"
	"\010\215readonly"
	"\005\016false"
	"\002\017fg"
	"\007\020getopts"
	"\004\021hash"
	"\002\022fc"
	"\005\023jobid"
	"\004\024jobs"
	"\004\025kill"
	"\005\026local"
	"\006\027printf"
	"\003\030pwd"
	"\004\031read"
	"\006\232return"
	"\003\233set"
	"\006\034setvar"
	"\005\235shift"
	"\004\036test"
	"\001\036["
	"\005\237times"
	"\004\240trap"
	"\001\241:"
	"\004\041true"
	"\004\042type"
	"\006\043ulimit"
	"\005\044umask"
	"\007\045unalias"
	"\005\246unset"
	"\004\047wait"
	"\007\050wordexp"
};
