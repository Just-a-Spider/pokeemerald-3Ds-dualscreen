#ifndef CTR_INPUT_CHORDS_H
#define CTR_INPUT_CHORDS_H

#include <stdint.h>
#include <stdbool.h>

void CtrChords_Update(void);
void CtrChords_NotifyYUsed(void);
uint16_t CtrChords_FilterGbaKeys(uint16_t held);
bool CtrChords_ConsumeStartToggle(void);

#endif
