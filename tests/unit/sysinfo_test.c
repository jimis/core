#include <test.h>

#include <mock.h>


/* Include C file to test static functions. */
#include <sysinfo.c>


static void test_uptime(void)
{
    /*
     * Assume we have been online at least five minutes, and less than two years.
     * If two years is not long enough, stop watching that uptime counter and
     * reboot the machine, dammit! :-)
     */
    int uptime = GetUptimeMinutes(time(NULL));
    printf("Uptime: %.2f days\n", uptime / (60.0 * 24));
    assert_in_range(uptime, 5, 60*24*365*2);
}

/**
 *  This test mocks the contents of /proc/1/cmdline.
 */
static void test_systemd_detection(void)
{
    EvalContext *ctx;
    Class *class;
    Mock *mock;

    printf("Testing where systemd SHOULD be detected...\n");

    ctx = EvalContextNew();
    mock = Mock_Filename("/proc/1/cmdline", "/sbin/systemd");
    OSClasses(ctx);                                     /* FUNCTION TO TEST */
    class = EvalContextClassGet(ctx, "default", "systemd");
    assert_true(class != NULL);
    Mock_End(mock);
    EvalContextDestroy(ctx);

    ctx = EvalContextNew();
    mock = Mock_Filename("/proc/1/cmdline", "/usr/lib/systemd/systemd");
    OSClasses(ctx);                                     /* FUNCTION TO TEST */
    class = EvalContextClassGet(ctx, "default", "systemd");
    assert_true(class != NULL);
    Mock_End(mock);
    EvalContextDestroy(ctx);
#if 0 //TODO
    Mock *mock2;
    ctx = EvalContextNew();
    mock  = Mock_Filename("/proc/1/cmdline", "/sbin/init");
    mock2 = Mock_realpath("/sbin/init", "/usr/lib/systemd/systemd");
    OSClasses(ctx);                                     /* FUNCTION TO TEST */
    class = EvalContextClassGet(ctx, "default", "systemd");
    assert_true(class != NULL);
    Mock_End(mock2);
    Mock_End(mock);
    EvalContextDestroy(ctx);
#endif
    printf("Testing where systemd SHOULD NOT be detected...\n");

    ctx = EvalContextNew();
    mock = Mock_Filename("/proc/1/cmdline", "/sbin/systemd-blah");
    OSClasses(ctx);                                     /* FUNCTION TO TEST */
    class = EvalContextClassGet(ctx, "default", "systemd");
    assert_true(class == NULL);
    Mock_End(mock);
    EvalContextDestroy(ctx);

    ctx = EvalContextNew();
    mock = Mock_Filename("/proc/1/cmdline", "/sbin/blah-systemd");
    OSClasses(ctx);                                     /* FUNCTION TO TEST */
    class = EvalContextClassGet(ctx, "default", "systemd");
    assert_true(class == NULL);
    Mock_End(mock);
    EvalContextDestroy(ctx);
#if 0 //TODO
    ctx = EvalContextNew();
    mock  = Mock_Filename("/proc/1/cmdline", "/sbin/systemd");
    mock2 = Mock_realpath("/sbin/systemd", "/sbin/init");
    OSClasses(ctx);                                     /* FUNCTION TO TEST */
    class = EvalContextClassGet(ctx, "default", "systemd");
    assert_true(class != NULL);
    Mock_End(mock2);
    Mock_End(mock);
    EvalContextDestroy(ctx);
#endif
}


int main()
{
    PRINT_TEST_BANNER();
    const UnitTest tests[] =
    {
        unit_test(test_uptime),
        unit_test(test_systemd_detection)
    };

    return run_tests(tests);
}
