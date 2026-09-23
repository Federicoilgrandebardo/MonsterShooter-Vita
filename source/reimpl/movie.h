#ifndef MOVIE_H
#define MOVIE_H

// Filmati del gioco con sceAvPlayer. Tutto arriva dal thread GL: PlayMovie,
// IsMoviePlaying ed eglSwapBuffers (verificato con la sonda sul ferro).
int  movie_start(const char *name); // nome senza estensione, 1 se il film parte
int  movie_is_playing(void);        // a fine film chiude il player e torna 0
void movie_draw(void);              // subito prima di eglSwapBuffers
int  movie_active(void);            // come is_playing, senza effetti collaterali

#endif
