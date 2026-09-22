#ifndef SOLOADER_FILELOG_H
#define SOLOADER_FILELOG_H

// Scrive i log anche su DATA_PATH "log.txt", cosi' si possono recuperare via FTP
// senza dipendere da PrincessLog, dal firewall del PC o dalla rete.
void filelog_reset(void);
void filelog_write(const char * text);

// Come sceClibPrintf, ma finisce anche su file. Serve ai log di so_util, che
// altrimenti parlerebbero solo sul canale di rete.
void filelog_printf(const char * fmt, ...);

#endif
