

#include	"lsq_Bounds.h"
#include	"lsq_Globals.h"
#include	"lsq_LoadPoints.h"
#include	"lsq_Msg.h"
#include	"lsq_Untwist.h"
#include	"lsq_XArray.h"

#include	"Cmdline.h"
#include	"Disk.h"
#include	"Maths.h"
#include	"Memory.h"

#include	<stdlib.h>
#include	<string.h>


/* --------------------------------------------------------------- */
/* CArgs --------------------------------------------------------- */
/* --------------------------------------------------------------- */

class CArgs {

public:
	char		tempdir[2048];	// master workspace
	const char	*prior;			// start from these solutions
	int			zilo,			// my output range
				zihi,
				zolo,			// extended input range
				zohi,
				zpernode;		// max layers per node
	bool		catclr,			// remake point catalog
				catonly,
				untwist;		// iff prior are affines

public:
	CArgs()
	{
		tempdir[0]	= 0;
		prior		= NULL;
		zilo		= 0;
		zihi		= 0;
		zolo		= -1;
		zohi		= -1;
		zpernode	= 200;
		catclr		= false;
		catonly		= false;
		untwist		= false;
	};

	void SetCmdLine( int argc, char* argv[] );
};

/* --------------------------------------------------------------- */
/* Statics ------------------------------------------------------- */
/* --------------------------------------------------------------- */

static CArgs	gArgs;






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

	printf( "\n---- Read params ----\n" );

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
		else if( GetArgStr( prior, "-prior=", argv[i] ) )
			printf( "Prior solutions: '%s'.\n", prior );
		else if( GetArg( &wkid, "-wkid=%d", argv[i] ) )
			printf( "wkid %d\n", wkid );
		else if( GetArg( &nwks, "-nwks=%d", argv[i] ) )
			printf( "nwks %d\n", nwks );
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
		else if( IsArg( "-catclr", argv[i] ) )
			catclr = true;
		else if( IsArg( "-catonly", argv[i] ) )
			catonly = true;
		else if( IsArg( "-untwist", argv[i] ) )
			untwist = true;
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

		if( !prior ) {
			printf( "Solving a stack requires -prior option.\n" );
			exit( 42 );
		}

		if( !DskExists( prior ) ) {
			printf( "Priors not found [%s].\n", prior );
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
// Look at most 10 layers from each end (12 for safety).
//
static int ZoFromZi(
	int	&zolo,
	int	&zohi,
	int	zilo_icat,
	int	zihi_icat )
{
	int	imax;

// zolo: lowest z that any interior layer touches

	zolo = *vL[zilo_icat].zdown.begin();
	imax = min( zilo_icat + 12, vL.size() - 1 );

	for( int icat = imax; icat > zilo_icat; --icat ) {

		int z = *vL[icat].zdown.begin();

		if( z < zolo )
			zolo = z;
	}

// zohi: highest z that touches interior

	int	zihi = vL[zihi_icat].z;

	imax = min( zihi_icat + 12, vL.size() - 1 );

	for( int icat = imax; icat > zihi_icat; --icat ) {

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

	nwks = nL / gArgs.zpernode;

	if( nL - nwks * gArgs.zpernode > 0 )
		++nwks;

	if( nwks <= 1 )
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

	for( int iw = 1; iw < nwks; ++iw ) {

		zilo_icat = zihi_icat + 1;
		zihi_icat = min( zilo_icat + gArgs.zpernode, nL ) - 1;
		ZoFromZi( zolo, zohi, zilo_icat, zihi_icat );

		char	buf[1024];

		sprintf( buf, "qsub -N lsq-%d -cwd -V -b y -pe batch 16"
		" 'lsq -temp=%s -prior=%s -wkid=%d -nwks=%d"
		" -zi=%d,%d -zo=%d,%d'",
		iw,
		gArgs.tempdir, gArgs.prior, iw, nwks,
		vL[zilo_icat].z, vL[zihi_icat].z, zolo, zohi );

		system( buf );
	}

	vL.resize( newcatsiz );
}

/* --------------------------------------------------------------- */
/* main ---------------------------------------------------------- */
/* --------------------------------------------------------------- */

// This flow is followed by master process (wkid==0)
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
		gArgs.zolo, gArgs.zohi, gArgs.catclr );

	if( gArgs.catonly ) {
		VMStats( stdout );
		return 0;
	}

/* ------------------------ */
/* Master partitions layers */
/* ------------------------ */

	if( !wkid )
		MasterLaunchWorkers();

/* ----------------- */
/* Load initial data */
/* ----------------- */

	InitTables( gArgs.zilo, gArgs.zihi );

	{
		CLoadPoints	*LP = new CLoadPoints;
		LP->Load( gArgs.tempdir );
		delete LP;
	}

/* ----------- */
/* Synchronize */
/* ----------- */

	MsgSyncWorkers( wkid, nwks );

/* ----- */
/* Start */
/* ----- */

	printf( "\n---- Development ----\n" );

	XArray	A;
	A.Load( gArgs.prior );
	UntwistAffines( A );
	A.Save();

//	DBox	B;
//	Bounds( B, A );

/* ---- */
/* Done */
/* ---- */

	VMStats( stdout );

	return 0;
}

