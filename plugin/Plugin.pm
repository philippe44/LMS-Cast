package Plugins::CastBridge::Plugin;

use strict;

use Data::Dumper;
use File::Spec::Functions;
use XML::Simple;
use base qw(Slim::Plugin::Base);

use Slim::Utils::Prefs;
use Slim::Utils::Log;

my $prefs = preferences('plugin.castbridge');

$prefs->init({ autorun => 0, opts => '', debugs => '', logging => 0, bin => undef, configfile => "castbridge.xml", profilesURL => initProfilesURL(), autosave => 1, eraselog => 0});

my $log = Slim::Utils::Log->addLogCategory({
	'category'     => 'plugin.castbridge',
	'defaultLevel' => 'WARN',
	'description'  => Slim::Utils::Strings::string('PLUGIN_CASTUPNPBRIDGE'),
}); 

sub initPlugin {
	my $class = shift;

	$class->SUPER::initPlugin(@_);
		
	require Plugins::CastBridge::Squeeze2cast;		
	
	if ($prefs->get('autorun')) {
		Plugins::CastBridge::Squeeze2cast->start;
	}
	
	if (!$::noweb) {
		require Plugins::CastBridge::Settings;
		Plugins::CastBridge::Settings->new;
		Slim::Web::Pages->addPageFunction("^castbridge-log.log", \&Plugins::CastBridge::Squeeze2cast::logHandler);
		Slim::Web::Pages->addPageFunction("^castbridge-config.xml", \&Plugins::CastBridge::Squeeze2cast::configHandler);
		Slim::Web::Pages->addPageFunction("castbridge/userguide.htm", \&Plugins::CastBridge::Squeeze2cast::guideHandler);
	}
	
	$log->warn(Dumper(Slim::Utils::OSDetect::details()));
}

sub initProfilesURL {
	my $file = catdir(Slim::Utils::PluginManager->allPlugins->{'CastBridge'}->{'basedir'}, 'install.xml');
	return XMLin($file, ForceArray => 0, KeepRoot => 0, NoAttr => 0)->{'profilesURL'};
}

sub shutdownPlugin {
	if ($prefs->get('autorun')) {
		Plugins::CastBridge::Squeeze2cast->stop;
	}
}

1;
