#!/usr/bin/perl -w

%allconfigs = ();
%common = ();

print "Reading config's ...\n";

for $config (@ARGV) {
	# Only config.*
	next if $config !~ /^config\..*/;
	# Nothing that is disabled, or remnant
	next if $config =~ /.*\.(default|disabled|stub)$/;

	%{$allconfigs{$config}} = ();

	print "  processing $config ... ";

	open(CONFIG, "< $config");

	while (<CONFIG>) {
		# Skip comments
		/^#*\s*CONFIG_(\w+)[\s=](.*)$/ or next;

		${$allconfigs{$config}}{$1} = $2;

		$common{$1} = $2;
	}

	close(CONFIG);

	print "done.\n";
}

print "\n";

print "Merging lists ... \n";

# %options - pointer to flavour config inside the allconfigs array
for $config (keys(%allconfigs)) {
	my %options = %{$allconfigs{$config}};

	print "   processing $config ... ";

	for $key (keys(%common)) {
		next if not defined $common{$key};

		# If we don't have the common option, then it isn't
		# common. If we do have that option, it must have the same
		# value.  EXCEPT where this file does not have a value at all
		# which may safely be merged with any other value; the value
		# will be elided during recombination of the parts.
		if (!defined($options{$key})) {
			# Its ok really ... let it merge
		} elsif (not defined($options{$key})) {
			undef $common{$key};
		} elsif ($common{$key} ne $options{$key}) {
			undef $common{$key};
		}
	}

	print "done.\n";
}

print "\n";

print "Creating common config ... ";

open(COMMON, "> config.common");
print COMMON "#\n# Common config options automatically generated by splitconfig.pl\n#\n";

for $key (sort(keys(%common))) {
	if (not defined $common{$key}) {
		print COMMON "# CONFIG_$key is UNMERGABLE\n";
	} elsif ($common{$key} eq "is not set") {
		print COMMON "# CONFIG_$key is not set\n";
	} else {
		print COMMON "CONFIG_$key=$common{$key}\n";
	}
}
close(COMMON);

print "done.\n\n";

print "Creating stub configs ...\n";

for $config (keys(%allconfigs)) {
	my %options = %{$allconfigs{$config}};

	print "  processing $config ... ";

	open(STUB, "> $config");
	print STUB "#\n# Config options for $config automatically generated by splitconfig.pl\n#\n";

	for $key (sort(keys(%options))) {
		next if defined $common{$key};

		if ($options{$key} =~ /^is /) {
			print STUB "# CONFIG_$key $options{$key}\n";
		} else {
			print STUB "CONFIG_$key=$options{$key}\n";
		}
	}

	close(STUB);

	print "done.\n";
}