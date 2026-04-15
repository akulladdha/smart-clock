#pragma once

void networkInit();     // WiFi + NTP setup (called from setup())
void loadAlarmSettings();
void saveAlarmSettings();
int  fetchStrategy();
void sendMorningResult(int strategyIdx, bool woke, float responseTimeSecs);
void registerRoutes();  // registers all WebServer routes