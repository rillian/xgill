#!/usr/bin/perl -w

use strict;
use File::Copy;

print "Content-type: text/plain\n\n";

opendir(DIR, "jobs_todo");
my @dirlist = readdir(DIR);
closedir(DIR);

foreach my $dirfile (@dirlist) {
    next if (!($dirfile =~ /\.job$/));

    # atomic move into the output directory.
    move("jobs_todo/$dirfile", "jobs") or next;

    open(IN, "< jobs/$dirfile");
    print <IN>;
    close(IN);

    exit;
}
