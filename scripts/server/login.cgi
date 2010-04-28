#!/usr/bin/perl -w

use strict;
use CGI qw(:standard);

sub exit_with {
    my ($msg) = @_;

    print("document.getElementById('login_server').innerHTML = '$msg';");
    exit;
}

print "Content-type: text/javascript\n\n";

my $mail = param('mail');
my $pass = param('pass');

exit_with "Need CGI parameter: mail" if (not (defined $mail));
exit_with "Need CGI parameter: pass" if (not (defined $pass));

if ($mail eq "") {
    exit_with "ERROR: Email is blank";
}

if ($mail =~ /[^\@\.a-zA-Z0-9_]/) {
    exit_with "ERROR: Bad character in email address";
}

if ($pass eq "") {
    exit_with "ERROR: Password is blank";
}

if ($pass =~ /[^\.a-zA-Z0-9_ ]/) {
    exit_with "ERROR: Bad character in password";
}

exit_with("ERROR: Unknown email/password") if (not -e "users");

open(USERS, "< users");

while (my $line = <USERS>) {
    chomp $line;
    $line =~ /^([^;]*);([^;]*);(.*)$/ or next;
    if ($1 eq $mail && crypt($pass, $3) eq $3) {
	print "var date = new Date();\n";
	print "date.setTime(date.getTime() + 90*24*60*60*1000);\n";
	print "document.cookie = 'sixgill=$mail:$pass; expires=' + date.toGMTString() + '; path=/';\n";
	print "setupLogin();\n";
	exit;
    }
}

close(USERS);

exit_with("ERROR: Unknown email/password");
