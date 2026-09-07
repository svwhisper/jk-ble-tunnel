#define main previous_test_main
#include "../../../tools/host_test_verify.c"
#undef main
int main(void)
{
    state_cache_init();
    s_vfy_done=true;
    test_now=100000000;
    memset(s_last_link,LINK_REACHABLE_IDLE,sizeof(s_last_link));
    /* enum array needs element assignment, not byte-fill. */
    for (unsigned i=0;i<CFG_NUM_UNITS;i++) s_last_link[i]=LINK_REACHABLE_IDLE;
    state_set_app_connected(1,true,test_now);
    polls=0; maintenance_tick();
    assert(polls==2);
    puts("REPRODUCED: one app demand emits two independent connect-driving polls in one tick");

    jk_cell_info_t cells={0}; state_set_cells(1,&cells);
    state_set_link_state(1,LINK_UP,true,test_now+1);
    maintain_at(test_now+1000000);
    unsigned old_polls=polls;
    state_set_link_state(1,LINK_REACHABLE_IDLE,false,0);
    state_set_link_state(1,LINK_UP,true,test_now+1);
    maintain_at(test_now+1000000);
    assert(polls==old_polls);
    puts("REPRODUCED: UP-DOWN-UP between ticks produces no new bootstrap");

    state_set_app_connected(1,false,test_now+1);
    maintain_at(test_now+61000000);
    unsigned first=releases; assert(first==1);
    maintain_at(test_now+1000000);
    assert(releases==first+1);
    puts("REPRODUCED: idle teardown enqueued again while prior teardown remains outstanding");
    return 0;
}
