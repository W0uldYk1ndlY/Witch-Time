#ifndef WT_SAVE_EXPORT_H
#define WT_SAVE_EXPORT_H
/* root is supplied only by Android getExternalFilesDir, never an intent path. */
int wt_save_export(const char *root, const char *run_id, const char *digest);
#endif
