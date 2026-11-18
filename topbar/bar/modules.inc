// ---- The module list: collapsed content, alerts, and the dashboard pages (sorted by DashboardOrder) ----
IModule* AlertsModuleInstance();  // topbar/alerts/alerts.inc
IModule* TransfersModuleInstance();  // topbar/bar/transfers.inc: PCTracker phone <-> PC transfers
IModule* const g_modules[] = {&g_clockModule, &g_mediaModule, AlertsModuleInstance(), &g_idlePage, &g_calendarPage, &g_weatherPage, &g_hardwarePage, &g_timerModule, &g_shelf, TransfersModuleInstance()};
