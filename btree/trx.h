

FDF_status_t	trxstart( struct FDF_thread_state *),
		trxcommit( struct FDF_thread_state *),
		trxrollback( struct FDF_thread_state *),
		trxquit( struct FDF_thread_state *);
uint64_t	trxid( struct FDF_thread_state *);
