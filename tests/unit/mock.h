#ifndef CFENGINE_MOCK_H
#define CFENGINE_MOCK_H


struct mock {
    bool   active;
    void  *pointer;
    char  *filename;
    char  *file_content;
    size_t file_content_len;
    size_t file_position;
    int    file_descriptor;
};
typedef struct mock Mock;


Mock *Mock_Filename(const char *filename, const char *content);
void Mock_End(Mock *mock);


#endif  /* CFENGINE_MOCK_H */
