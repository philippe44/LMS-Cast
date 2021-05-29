package Plugins::CastBridge::Plugin;

use strict;

use Data::Dumper;
use File::Spec::Functions;
use XML::Simple;
use base qw(Slim::Plugin::Base);

use Slim::Utils::Prefs;
use Slim::Utils::Log;
use Slim::Control::Request;

my $prefs = preferences('plugin.castbridge');
my $hasOutputChannels;
my $statusHandler = Slim::Control::Request::addDispatch(['status', '_index', '_quantity'], [1, 1, 1, \&statusQuery]);

$prefs->init({ 
	autorun => 0, 
	opts => '', 
	debugs => '', 
	logging => 0, 
	bin => undef, 
	configfile => "castbridge.xml", 
	profilesURL => initProfilesURL(), 
	autosave => 1, 
	eraselog => 0,
	baseport => '', 
});

my $log = Slim::Utils::Log->addLogCategory({
	'category'     => 'plugin.castbridge',
	'defaultLevel' => 'WARN',
	'description'  => Slim::Utils::Strings::string('PLUGIN_CASTBRIDGE'),
}); 

sub hasOutputChannels {
	my ($self) = @_;
	return $hasOutputChannels->(@_) unless $self->modelName =~ /CastBridge/;
	return 0;
}

sub initPlugin {
	my $class = shift;

	# this is hacky but I won't redefine a whole player model just for this	
	require Slim::Player::SqueezePlay;
	$hasOutputChannels = Slim::Player::SqueezePlay->can('hasOutputChannels');
	*Slim::Player::SqueezePlay::hasOutputChannels = \&hasOutputChannels;

	$class->SUPER::initPlugin(@_);
	
	Plugins::CastBridge::Queries::initQueries();
		
	require Plugins::CastBridge::Squeeze2cast;		
	
	if ($prefs->get('autorun')) {
		Plugins::CastBridge::Squeeze2cast->start;
	}
	
	if (!$::noweb) {
		require Plugins::CastBridge::Settings;
		Plugins::CastBridge::Settings->new;
		Slim::Web::Pages->addPageFunction("^castbridge-log.log", \&Plugins::CastBridge::Squeeze2cast::logHandler);
		Slim::Web::Pages->addPageFunction("^castbridge-config.xml", \&Plugins::CastBridge::Squeeze2cast::configHandler);
		Slim::Web::Pages->addPageFunction("castbridge-guide.htm", \&Plugins::CastBridge::Squeeze2cast::guideHandler);
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

sub statusQuery {
	my ($request) = @_;
	my $song = $request->client->playingSong if $request->client;

	$statusHandler->($request);
	
	my $song = $request->client->playingSong if $request->client;
	return unless $song;
	
	my $handler = $song->currentTrackHandler;
	return unless $handler;
	
	$request->addResult("repeating_stream", 1) if $handler->can('isRepeatingStream') && $handler->isRepeatingStream($song);
}

1;
