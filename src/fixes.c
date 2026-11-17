#include "fix.h"

extern const WtFix fix_nearestcell, fix_savelist, fix_savesort, fix_gcload, fix_splinecopy, fix_consts, fix_battleblur,
    fix_battlefog, fix_battlesound, fix_battlecull, fix_friendlyview, fix_unithud, fix_live, fix_overlay, fix_dlccheck,
    fix_shadowcache, fix_fxlookat, fix_subskin, fix_dlss, fix_deferdraw;

const WtFix *const wt_fixes[] = {&fix_nearestcell, &fix_savelist,     &fix_gcload,      &fix_splinecopy, &fix_consts,
                                 &fix_battleblur,  &fix_friendlyview, &fix_unithud,     &fix_live,       &fix_overlay,
                                 &fix_battlefog,   &fix_battlesound,  &fix_battlecull,  &fix_dlccheck,   &fix_shadowcache,
                                 &fix_fxlookat,    &fix_subskin,      &fix_savesort,    &fix_dlss};
const int wt_nfixes = sizeof wt_fixes / sizeof wt_fixes[0];
const WtFix *const wt_late_fixes[] = {&fix_deferdraw};
const int wt_nlate_fixes = sizeof wt_late_fixes / sizeof wt_late_fixes[0];
