#pragma once

#include "app_state.h"

#include <string>

void Log(AppState& app, const std::string& line);
void StartSender(AppState& app);
void StopSender(AppState& app);
