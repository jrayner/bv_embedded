#!/usr/bin/perl
use Net::Telnet;

## You may need to do 'sudo apt-get install libnet-telnet-perl' first

## To use it openocd should already be running and in the directory containing the file

$numArgs = $#ARGV + 1;
if($numArgs != 1){
    die( "Usage ./do_flash.pl [main.bin] \n");
}

$file   = $ARGV[0];

$ip = "127.0.0.1"; 
$port = 4444;

$telnet = new Net::Telnet (
    Port   => $port,
    Timeout=>10,
    Errmode=>'die',
    Prompt =>'/>/');

$telnet->open($ip);

print $telnet->cmd('reset halt');
print $telnet->cmd('flash probe 0');
print $telnet->cmd('stm32x mass_erase 0');
print $telnet->cmd('flash write_bank 0 '.$file.' 0');
print $telnet->cmd('reset run');
print $telnet->cmd('exit');

print "\n";
