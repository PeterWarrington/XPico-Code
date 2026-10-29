/* Resources for the Mac OS 9 / Carbon build.
 *
 * This replaces Retro68's stock RetroCarbonAPPL.r for one reason: its SIZE
 * resource asks for a 1 MB memory partition, and the editor needs far more.
 * Every panel paints through an offscreen GWorld the size of itself, and the
 * editor's own buffer at a usable window size is width * height * 4 bytes --
 * around 1.5 MB on its own. NewGWorld then failed, nothing painted, and with
 * the heap exhausted even the Dialog Manager could not build an alert, so the
 * window and every dialog came up blank.
 */
#include "Types.r"
#include "Processes.r"
#include "Finder.r"
#include "CodeFragments.r"

#ifndef CFRAG_NAME
#define CFRAG_NAME "XPico Code"
#endif

resource 'cfrg' (0) {
	{
		kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
		kDefaultStackSize, kNoAppSubFolder,
		kApplicationCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
		CFRAG_NAME
	}
};

type 'carb' {
};

resource 'carb' (0) {
};

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	/* preferred, then minimum. Enough for the offscreen buffers at a full
	 * screen window, the scrollback, and room for the Toolbox to work in. */
	16384 * 1024,
	6144 * 1024
};

/* ---- Finder icon wiring ----
 *
 * The icon family in icons.r is only shown once the Finder can tie it to this
 * application: a signature resource whose type is the creator code, an FREF
 * naming the file type it belongs to, and a BNDL joining the two. Without the
 * BNDL the app shows the generic application icon no matter what icons it
 * carries. The creator 'XPCD' matches the -c flag in tools/build_mac.sh. */

#include "icons.r"

/* The signature resource. Its contents are the version string the Finder
 * shows in Get Info. */
type 'XPCD' as 'STR ';
resource 'XPCD' (0, purgeable) {
	"XPico Code 1.1.0"
};

resource 'FREF' (128) {
	'APPL',                 /* this bundle describes the application itself */
	0,                      /* local icon list ID, matched by the BNDL below */
	""
};

resource 'BNDL' (128) {
	'XPCD',                 /* owner signature */
	0,
	{
		'ICN#', { 0, 128 },   /* local ID 0 -> the icon family at 128 */
		'FREF', { 0, 128 }
	}
};
