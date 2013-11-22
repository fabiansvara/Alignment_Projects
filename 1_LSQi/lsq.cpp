

#include	"lsq_Globals.h"
#include	"lsq_LoadPoints.h"
#include	"lsq_Msg.h"

#include	"Cmdline.h"
#include	"Disk.h"
#include	"Maths.h"
#include	"Memory.h"
#include	"Timer.h"

#include	<stdlib.h>
#include	<string.h>


/* --------------------------------------------------------------- */
/* CArgs --------------------------------------------------------- */
/* --------------------------------------------------------------- */

class CArgs {

public:
	char	tempdir[2048],	// master workspace
			*priorafftbl;	// start affine model from these
	int		wkid,			// my worker id (main=0)
			zilo,			// my output range
			zihi,
			zolo,			// extended input range
			zohi,
			zpernode;		// max layers per node
	bool	clrcat;			// remake point catalog

public:
	CArgs()
	{
		tempdir[0]	= 0;
		priorafftbl	= NULL;
		wkid		= 0;
		zilo		= 0;
		zihi		= 0;
		zolo		= -1;
		zohi		= -1;
		zpernode	= 200;
		clrcat		= false;
	};

	void SetCmdLine( int argc, char* argv[] );
};

/* --------------------------------------------------------------- */
/* Statics ------------------------------------------------------- */
/* --------------------------------------------------------------- */

static CArgs	gArgs;
static int		gnw = 1;






/* --------------------------------------------------------------- */
/* SetCmdLine ---------------------------------------------------- */
/* --------------------------------------------------------------- */

void CArgs::SetCmdLine( int argc, char* argv[] )
{
// Name log by worker

	for( int i = 1; i < argc; ++i ) {
		if( GetArg( &wkid, "-wkid=%d", argv[i] ) )
			break;
	}

	char slog[32];
	sprintf( slog, "lsq_%d.txt", wkid );
	freopen( slog, "w", stdout );

// Parse command line args

	printf( "---- Read params ----\n" );

	if( argc < 3 ) {
		printf(
		"Usage: lsq -temp=path -zi=i,j [options].\n" );
		exit( 42 );
	}

	vector<int>	vi;

	for( int i = 1; i < argc; ++i ) {

		const char	*instr;

		if( GetArgStr( instr, "-temp=", argv[i] ) ) {

			DskAbsPath( tempdir, sizeof(tempdir), instr, stdout );
			printf( "Temp dir: '%s'.\n", tempdir );
			GetIDB( tempdir );
		}
		else if( GetArgStr( instr, "-prior=", argv[i] ) ) {

			priorafftbl = strdup( instr );
			printf( "Prior solutions: '%s'.\n", priorafftbl );
		}
		else if( GetArg( &wkid, "-wkid=%d", argv[i] ) )
			printf( "wkid %d\n", wkid );
		else if( GetArgList( vi, "-zi=", argv[i] ) ) {

			if( 2 == vi.size() ) {
				zilo = vi[0];
				zihi = vi[1];
				printf( "zi [%d %d]\n", zilo, zihi );
			}
			else {
				printf( "Bad format in -zi [%s].\n", argv[i] );
				exit( 42 );
			}
		}
		else if( GetArgList( vi, "-zo=", argv[i] ) ) {

			if( 2 == vi.size() ) {
				zolo = vi[0];
				zohi = vi[1];
				printf( "zo [%d %d]\n", zolo, zohi );
			}
			else {
				printf( "Bad format in -zo [%s].\n", argv[i] );
				exit( 42 );
			}
		}
		else if( GetArg( &zpernode, "-zpernode=%d", argv[i] ) )
			;
		else if( IsArg( "-clrcat", argv[i] ) )
			clrcat = true;
		else {
			printf( "Did not understand option '%s'.\n", argv[i] );
			exit( 42 );
		}
	}

	if( zolo == -1 ) {
		zolo = zilo;
		zohi = zihi;
	}

	if( !wkid && zilo != zihi ) {

		if( !priorafftbl ) {
			printf( "Solving a stack requires prior affines.\n" );
			exit( 42 );
		}

		if( !DskExists( priorafftbl ) ) {
			printf( "Prior affines not found [%s].\n", priorafftbl );
			exit( 42 );
		}
	}
}

/* --------------------------------------------------------------- */
/* ZoFromZi ------------------------------------------------------ */
/* --------------------------------------------------------------- */

// Determine dependency range [zolo,zohi] and return cat index
// of zohi (so master can truncate it's own list).
//
static int ZoFromZi(
	int	&zolo,
	int	&zohi,
	int	zilo_icat,
	int	zihi_icat )
{
	zolo = *vL[zilo_icat].zdown.begin();

// Look up to 10 layers away (12 for safety)

	int	zihi = vL[zihi_icat].z;
	int	imax = min( zihi_icat + 12, vL.size() - 1 );

	for( int icat = imax; icat > zihi_icat + 1; --icat ) {

		if( *vL[icat].zdown.begin() <= zihi ) {

			zohi = vL[icat].z;
			return icat;
		}
	}

// Default

	zohi = zihi;
	return zihi_icat;
}

/* --------------------------------------------------------------- */
/* MasterLaunchWorkers ------------------------------------------- */
/* --------------------------------------------------------------- */

static void MasterLaunchWorkers()
{
// How many workers?

	int	nL = vL.size();

	gnw = nL / gArgs.zpernode;

	if( nL - gnw * gArgs.zpernode > 0 )
		++gnw;

	if( gnw <= 1 )
		return;

// Master will be lowest block

	printf( "\n---- Launching workers ----\n" );

	int	zolo,
		zohi,
		zilo_icat = 0,
		zihi_icat = gArgs.zpernode - 1,
		newcatsiz = ZoFromZi( zolo, zohi, zilo_icat, zihi_icat ) + 1;

		gArgs.zihi = vL[zihi_icat].z;
		gArgs.zohi = zohi;

		printf( "Master own range zi [%d %d] zo [%d %d]\n",
		gArgs.zilo, gArgs.zihi, gArgs.zolo, gArgs.zohi );

// Make ranges

	MsgClear();

	for( int iw = 1; iw < gnw; ++iw ) {

		zilo_icat = zihi_icat + 1;
		zihi_icat = min( zilo_icat + gArgs.zpernode, nL ) - 1;
		ZoFromZi( zolo, zohi, zilo_icat, zihi_icat );

		char	buf[1024];

		sprintf( buf, "qsub -N lsq-%d -cwd -V -b y -pe batch 16"
		" 'lsq -temp=%s -prior=%s -wkid=%d -zi=%d,%d -zo=%d,%d'",
		iw,
		gArgs.tempdir, gArgs.priorafftbl,
		iw,
		vL[zilo_icat].z, vL[zihi_icat].z,
		zolo, zohi );

		system( buf );
	}

	vL.resize( newcatsiz );
}

/* --------------------------------------------------------------- */
/* main ---------------------------------------------------------- */
/* --------------------------------------------------------------- */

// This flow is followed by master process (gArgs.wkid==0)
// and all workers process.
//
int main( int argc, char **argv )
{
	clock_t	t0;

/* ---------------------- */
/* All parse command line */
/* ---------------------- */

	gArgs.SetCmdLine( argc, argv );

/* ---------------------- */
/* All need their catalog */
/* ---------------------- */

	LayerCat( vL, gArgs.tempdir,
		gArgs.zolo, gArgs.zohi, gArgs.clrcat );

/* ------------------------ */
/* Master partitions layers */
/* ------------------------ */

	if( !gArgs.wkid )
		MasterLaunchWorkers();

/* ----------------- */
/* Load initial data */
/* ----------------- */

	CLoadPoints	LP;

	LP.Load( gArgs.tempdir, (gArgs.zolo != gArgs.zohi) );

/* ---- */
/* Done */
/* ---- */

	VMStats( stdout );
	return 0;
}

