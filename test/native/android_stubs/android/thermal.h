#pragma once
struct AThermalManager; typedef struct AThermalManager AThermalManager;
typedef enum { ATHERMAL_STATUS_NONE = 0, ATHERMAL_STATUS_LIGHT, ATHERMAL_STATUS_MODERATE, ATHERMAL_STATUS_SEVERE, ATHERMAL_STATUS_CRITICAL } AThermalStatus;
AThermalManager* AThermal_acquireManager();
void AThermal_releaseManager(AThermalManager*);
AThermalStatus AThermal_getCurrentThermalStatus(AThermalManager*);
