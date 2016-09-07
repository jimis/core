


bool share_connection(const char * path, const int descriptor,
                      const char *message);
int take_connection(const int uds, char **message);
