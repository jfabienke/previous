/*
  Previous - automation.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_AUTOMATION_H
#define PREV_AUTOMATION_H

#ifdef __cplusplus
extern "C" {
#endif

extern void Automation_Init(void);
extern void Automation_Run(int nHostCycles);
/* Set by "snap", taken by a board that keeps snapshots (nd_rust.cpp) */
extern volatile int Automation_SnapshotRequest;

#ifdef __cplusplus
}
#endif

#endif /* PREV_AUTOMATION_H */
