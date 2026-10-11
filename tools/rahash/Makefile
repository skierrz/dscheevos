# RetroAchievements DS hash of .nds files, computed by rcheevos (same code DScheevos uses).
RCHEEVOS ?= ../../external/rcheevos
rahash: rahash.c
	$(CC) -O2 -DRC_NO_THREADS -DRC_HASH_NO_DISC -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED \
	  -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src rahash.c \
	  $(RCHEEVOS)/src/hash/rc_hash.c $(RCHEEVOS)/src/hash/rc_hash_rom.c $(RCHEEVOS)/src/util/md5.c \
	  $(RCHEEVOS)/src/util/rc_compat.c $(RCHEEVOS)/src/util/rc_util.c -o $@
clean:
	rm -f rahash
