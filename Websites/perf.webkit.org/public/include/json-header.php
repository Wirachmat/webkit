<?php

require_once('db.php');
require_once('test-path-resolver.php');

header('Content-type: application/json');

function exit_with_error($status, $details = array()) {
    $details['status'] = $status;
    merge_additional_details($details);

    echo json_encode($details);
    exit(1);
}

function echo_success($details = array()) {
    $details['status'] = 'OK';
    merge_additional_details($details);

    echo json_encode($details);
}

function exit_with_success($details = array()) {
    echo_success($details);
    exit(0);
}

$additional_exit_details = array();

function set_exit_detail($name, $value) {
    global $additional_exit_details;
    $additional_exit_details[$name] = $value;
}

function merge_additional_details(&$details) {
    global $additional_exit_details;
    foreach ($additional_exit_details as $name => $value) {
        if (!array_key_exists($name, $details))
            $details[$name] = $value;
    }
}

function connect() {
    $db = new Database;
    if (!$db->connect())
        exit_with_error('DatabaseConnectionError');
    return $db;
}

function camel_case_words_separated_by_underscore($name) {
    return implode('', array_map('ucfirst', explode('_', $name)));
}

function require_format($name, $value, $pattern) {
    if (!preg_match($pattern, $value))
        exit_with_error('Invalid' . $name, array('value' => $value));
}

function require_existence_of($array, $list_of_arguments, $prefix = '') {
    if ($prefix)
        $prefix .= '_';
    foreach ($list_of_arguments as $key => $pattern) {
        $name = camel_case_words_separated_by_underscore($prefix . $key);
        if (!array_key_exists($key, $array))
            exit_with_error($name . 'NotSpecified');
        require_format($name, $array[$key], $pattern);
    }
}

function ensure_privileged_api_data() {
    global $HTTP_RAW_POST_DATA;

    if ($_SERVER['REQUEST_METHOD'] != 'POST')
        exit_with_error('InvalidRequestMethod');

    if (!isset($HTTP_RAW_POST_DATA))
        exit_with_error('InvalidRequestContent');

    $data = json_decode($HTTP_RAW_POST_DATA, true);

    if ($data === NULL)
        exit_with_error('InvalidRequestContent');

    return $data;
}

function ensure_privileged_api_data_and_token() {
    $data = ensure_privileged_api_data();
    if (!verify_token(array_get($data, 'token')))
        exit_with_error('InvalidToken');
    return $data;
}

function remote_user_name() {
    return array_get($_SERVER, 'REMOTE_USER');
}

function compute_token() {
    if (!array_key_exists('CSRFSalt', $_COOKIE) || !array_key_exists('CSRFExpiration', $_COOKIE))
        return NULL;
    $user = remote_user_name();
    $salt = $_COOKIE['CSRFSalt'];
    $expiration = $_COOKIE['CSRFExpiration'];
    return hash('sha256', "$salt|$user|$expiration");
}

function verify_token($token) {
    $expected_token = compute_token();
    return $expected_token && $token == $expected_token && $_COOKIE['CSRFExpiration'] > time();
}

function verify_slave($db, $params) {
    array_key_exists('slaveName', $params) or exit_with_error('MissingSlaveName');
    array_key_exists('slavePassword', $params) or exit_with_error('MissingSlavePassword');

    $slave_info = array(
        'name' => $params['slaveName'],
        'password_hash' => hash('sha256', $params['slavePassword'])
    );

    $matched_slave = $db->select_first_row('build_slaves', 'slave', $slave_info);
    if (!$matched_slave)
        exit_with_error('SlaveNotFound', array('name' => $slave_info['name']));
}

function find_triggerable_for_task($db, $task_id) {
    $task_id = intval($task_id);

    $test_rows = $db->query_and_fetch_all('SELECT metric_test AS "test", task_platform as "platform"
        FROM analysis_tasks JOIN test_metrics ON task_metric = metric_id WHERE task_id = $1', array($task_id));
    if (!$test_rows)
        return NULL;
    $target_test_id = $test_rows[0]['test'];
    $platform_id = $test_rows[0]['platform'];

    $path_resolver = new TestPathResolver($db);
    $test_ids = $path_resolver->ancestors_for_test($target_test_id);

    $results = $db->query_and_fetch_all('SELECT trigconfig_triggerable AS "triggerable", trigconfig_test AS "test"
        FROM triggerable_configurations WHERE trigconfig_platform = $1 AND trigconfig_test = ANY($2)',
        array($platform_id, '{' . implode(', ', $test_ids) . '}'));
    if (!$results)
        return NULL;

    $test_to_triggerable = array();
    foreach ($results as $row)
        $test_to_triggerable[$row['test']] = $row['triggerable'];

    foreach ($test_ids as $test_id) {
        $triggerable = array_get($test_to_triggerable, $test_id);
        if ($triggerable)
            return array('id' => $triggerable, 'test' => $test_id, 'platform' => $platform_id);
    }

    return NULL;
}

?>
