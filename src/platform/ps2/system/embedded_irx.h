#ifndef _EMBEDDED_IRX_H
#define _EMBEDDED_IRX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Look up an embedded IRX by basename (e.g. "CDVD.IRX"). The match is
   case-insensitive against the basename of the requested path, so
   "host:CDVD.IRX", "cdrom:\\CDVD.IRX", "rom0:CDVD.IRX" etc. all hit the
   same entry. Returns 0 and fills *out_data / *out_size on hit, or
   returns -1 if no embedded module matches. */
int  EmbeddedIrxFind(const char *path,
                     const unsigned char **out_data,
                     unsigned int         *out_size);

/* Loads an embedded IRX onto the IOP via SifExecModuleBuffer. Returns
   the module ID on success or a negative error on failure. */
int  EmbeddedIrxLoad(const unsigned char *data,
                     unsigned int         size,
                     int                  arg_len,
                     const char          *args);

#ifdef __cplusplus
}
#endif

#endif
