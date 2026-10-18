#include "request.h"

#include <stdio.h>
#include <string.h>

const char* request_post(const char* post, char* buf, size_t size)
{
  int n;
  if (!post || strncmp(post, "r=achievementsets&", 18) != 0)
    return post;
  n = snprintf(buf, size, "%s&f=3", post);
  return n > 0 && (size_t)n < size ? buf : post;
}
