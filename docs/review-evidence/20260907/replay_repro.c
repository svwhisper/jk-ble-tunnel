#define main previous_test_main
#include "../../../tools/host_test_b_cache.c"
#undef main
int main(void)
{
    nb_state_init(); test_now=100000000;
    nb_set_conn(1,true,7); nb_set_notify(1,true); nb_set_link(1,LINK_UP);
    nb_notify_session_t s; nb_get_notify_session(1,&s);
    nb_mark_replay(1,NB_REPLAY_SETTINGS|NB_REPLAY_CELLINFO,s.epoch);
    ++test_now;
    /* notify_session calls this on a devinfo header before allocation. */
    nb_note_dev_forwarded(1,s.epoch);
    uint8_t bits;
    assert(nb_claim_replay(1,&s,&bits)==2 && bits==0);
    puts("REPRODUCED: devinfo attempt cancels settings/cell debt without delivering either");
    return 0;
}
