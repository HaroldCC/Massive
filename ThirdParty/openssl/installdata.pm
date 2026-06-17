package OpenSSL::safe::installdata;

use strict;
use warnings;
use Exporter;
our @ISA = qw(Exporter);
our @EXPORT = qw(
    @PREFIX
    @libdir
    @BINDIR @BINDIR_REL_PREFIX
    @LIBDIR @LIBDIR_REL_PREFIX
    @INCLUDEDIR @INCLUDEDIR_REL_PREFIX
    @APPLINKDIR @APPLINKDIR_REL_PREFIX
    @MODULESDIR @MODULESDIR_REL_LIBDIR
    @PKGCONFIGDIR @PKGCONFIGDIR_REL_LIBDIR
    @CMAKECONFIGDIR @CMAKECONFIGDIR_REL_LIBDIR
    $COMMENT $VERSION @LDLIBS
);

our $COMMENT                    = '';
our @PREFIX                     = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl' );
our @libdir                     = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\lib' );
our @BINDIR                     = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\bin' );
our @BINDIR_REL_PREFIX          = ( 'bin' );
our @LIBDIR                     = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\lib' );
our @LIBDIR_REL_PREFIX          = ( 'lib' );
our @INCLUDEDIR                 = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\include' );
our @INCLUDEDIR_REL_PREFIX      = ( 'include' );
our @APPLINKDIR                 = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\include\openssl' );
our @APPLINKDIR_REL_PREFIX      = ( 'include/openssl' );
our @MODULESDIR                 = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\lib\ossl-modules' );
our @MODULESDIR_REL_LIBDIR      = ( 'ossl-modules' );
our @PKGCONFIGDIR               = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\lib' );
our @PKGCONFIGDIR_REL_LIBDIR    = ( '' );
our @CMAKECONFIGDIR             = ( 'F:\Dev\Massive\Build\ThirdParty\install\openssl\lib\cmake\OpenSSL' );
our @CMAKECONFIGDIR_REL_LIBDIR  = ( 'cmake\OpenSSL' );
our $VERSION                    = '4.0.1';
our @LDLIBS                     =
    # Unix and Windows use space separation, VMS uses comma separation
    $^O eq 'VMS'
    ? split(/ *, */, 'ws2_32.lib gdi32.lib advapi32.lib crypt32.lib user32.lib ')
    : split(/ +/, 'ws2_32.lib gdi32.lib advapi32.lib crypt32.lib user32.lib ');

1;
