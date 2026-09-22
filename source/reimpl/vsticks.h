/**
 * @file  vsticks.h
 * @brief Levette fisiche sugli stick virtuali del gioco.
 */

#ifndef SOLOADER_VSTICKS_H
#define SOLOADER_VSTICKS_H

void vsticks_init(void);

/// Va chiamata sul thread del gioco, dallo hook su AbstractApp::PrivateTick.
void vsticks_tick(void *app);

#endif // SOLOADER_VSTICKS_H
