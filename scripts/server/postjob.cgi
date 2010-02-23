#!/usr/bin/perl -w

use strict;
use CGI qw(:standard);
use File::Copy;

print "Content-type: text/plain\n\n";

my $report = param('report') or die;
my $ext = param('ext') or die;
my $message = param('message') or die;
my $html = param('html') or "";
my $file = param('file') or "";

die if ($ext =~ /[^a-zA-Z0-9_]/);

# note: we don't sanitize the message or html at all, and since we don't use
# authentication this script could be used by anyone to upload arbitary html.
# this should probably be fixed.

open(OUT, "> $ext.done");
print OUT "$message\n";
close(OUT);

if ($html ne "") {
    $report =~ /(.*?)\$/ or die;
    my $dir = $1;

    die if ($dir =~ /[^a-zA-Z0-9_]/);
    die if ($file =~ /[^a-zA-Z0-9_]/);

    open(HTML, "> $dir/$file.html");
    print HTML $html;
    close(HTML);
}

move("$ext.done", "jobs/$ext.done");
