--TEST--
Coms test communications implementation
--SKIPIF--
<?php
if (!getenv("DD_AGENT_HOST")) {
    die("skip test if agent host is not set");
}
?>
--FILE--
<?php
    putenv("DD_TRACE_DEBUG_CURL_OUTPUT=1");
    $spans = 100000;
    // $incorrectSpan = ["incorrect_span"];
    // for($i = 0; $i < $spans; $i++){
    //     break;
    //     dd_trace_flush_span(0, $incorrectSpan);
    // }
    dd_trace_internal_fn('set_writer_send_on_flush', true);
    dd_trace_internal_fn('synchronous_flush');

    echo "FLUSH without SEND" . PHP_EOL;

    $group_id = dd_trace_coms_next_span_group_id();
    echo "GROUP_ID " . $group_id . PHP_EOL;
    echo "NEXT GROUP_ID " . dd_trace_coms_next_span_group_id() . PHP_EOL;


    function blah(&$spans, $span) {
        $spans []= $span;
    }
    $cnt = 1589331357723252210;

    $spansToFlush = [];
    for($i =0 ; $i < $spans; $i++) {
        $span = [
            "trace_id" => 1589331357723252209,
            "span_id" => $cnt + i,
            "name" => "test_name",
            "resource" => "test_resource",
            "service" => "test_service",
            "start" => 1518038421211969000,
            "duration" => 1,
            "error" => 0,
        ];

        dd_trace_flush_span(0, $span);

        $span["span_id"]++;
        // blah($spansToFlush, $span);
    }
    // dd_trace_flush_span(0, $spansToFlush);

    echo "SPANS " . $spans . PHP_EOL;
    echo "SPAN_SIZE " . strlen(dd_trace_serialize_msgpack($span)) . PHP_EOL;

    dd_trace_internal_fn('set_writer_send_on_flush', true);
    dd_trace_internal_fn('synchronous_flush');
    dd_trace_internal_fn('shutdown_writer', true); //shuting down worker immediately will result in curl traces flush
?>
--EXPECTF--
FLUSH without SEND
GROUP_ID 1
NEXT GROUP_ID 2
SPANS 10000
SPAN_SIZE 127
{"rate_by_service":{"service:,env:":1}}
UPLOADED 1270%d bytes
