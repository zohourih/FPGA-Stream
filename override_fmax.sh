#!/bin/bash

skip=0

if [[ -z $1 ]]
then
	echo "Missing kernel file path!"
	exit -1
else
	path=$1
fi

if [[ -z $2 ]]
then
	skip=1
else
	echo -n "Overriding Fmax: "
	fmax=$2
fi

if [[ $skip -eq 0 ]]
then
	if [[ -z `quartus_fit --version | grep "Pro"` ]]
	then
		file=`echo $path/scripts/post_flow.tcl`
	else
		file=`echo $path/scripts/post_flow_pr.tcl`
	fi

	orig=`cat $file | grep adjust_plls.*tcl`
	if [[ -z "$orig" ]]
	then
		echo "FAILURE!! (pattern not found)"
		exit -1
	fi

	text=`echo "$orig" | sed 's/^[[:space:]]*//'`
	indent=${orig%"$text"}
	orig_fix=$(printf '%s\n' "$orig" | sed 's:[\/&]:\\&:g;$!s/$/\\/')

	new=`echo -e "$indent""set argv [list -fmax $fmax]\n""$indent""set argc 2\n""$orig\n""$indent""unset argv\n""$indent""unset argc"`
	new_fix=$(printf '%s\n' "$new" | sed 's:[\/&]:\\&:g;$!s/$/\\/')

	sed -i "s/$orig_fix/$new_fix/" $file
	echo "SUCCESS!!"
fi
