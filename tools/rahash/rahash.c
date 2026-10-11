/* Print the RetroAchievements hash rcheevos computes for a DS ROM. */
#include "rc_hash.h"
#include "rc_consoles.h"
#include <stdio.h>
int main(int argc, char** argv)
{
	char hash[33];
	rc_hash_iterator_t it;
	for (int i = 1; i < argc; i++) {
		rc_hash_initialize_iterator(&it, argv[i], NULL, 0);
		if (rc_hash_generate(hash, RC_CONSOLE_NINTENDO_DS, &it)) printf("%s  %s\n", hash, argv[i]);
		else printf("FAILED  %s\n", argv[i]);
		rc_hash_destroy_iterator(&it);
	}
	return 0;
}
