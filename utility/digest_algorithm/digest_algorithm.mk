NAME := digest_algorithm

$(NAME)_TYPE := share
$(NAME)_SOURCES := digest_algorithm.c CheckSumUtils.c crc.c crc16.c md5.c
GLOBAL_INCLUDES += . 
