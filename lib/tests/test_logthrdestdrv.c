/*
 * Copyright (c) 2018 Balabit
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "logthrdestdrv.h"
#include "apphook.h"

#include <criterion/criterion.h>
#include "grab-logging.h"
#include "stopwatch.h"
#include "cr_template.h"

typedef struct TestThreadedDestDriver
{
  LogThreadedDestDriver super;
  gint insert_counter;
  gint flush_counter;
  gint failure_counter;
  gint prev_flush_size;
  gint flush_size;
} TestThreadedDestDriver;

static const gchar *
_generate_persist_name(const LogPipe *s)
{
  return "persist-name";
}

static const gchar *
_format_stats_instance(LogThreadedDestDriver *s)
{
  return "stats-name";
}

static TestThreadedDestDriver *
test_threaded_dd_new(GlobalConfig *cfg)
{
  TestThreadedDestDriver *self = g_new0(TestThreadedDestDriver, 1);
  log_threaded_dest_driver_init_instance(&self->super, cfg);
  self->super.super.super.super.generate_persist_name = _generate_persist_name;
  self->super.format_stats_instance = _format_stats_instance;

  /* the insert function will be initialized explicitly in testcases */
  self->super.worker.insert = NULL;
  return self;
}

/* spins maximum about 10 seconds, if you need more time, increase the loop counter */

#define MAX_SPIN_ITERATIONS 10000

static void
_spin_for_counter_value(StatsCounterItem *counter, gssize expected_value)
{
  gssize value = stats_counter_get(counter);
  struct timespec sleep_time = { 0, 1000000 };
  gint c;

  while (value != expected_value && c < MAX_SPIN_ITERATIONS)
    {
      value = stats_counter_get(counter);
      nanosleep(&sleep_time, NULL);
      c++;
    }
  cr_assert(expected_value == value,
            "counter did not reach the expected value after %d seconds, expected_value=%" G_GSSIZE_FORMAT ", value=%" G_GSSIZE_FORMAT,
            MAX_SPIN_ITERATIONS / 1000, expected_value, value);
}

static void
_generate_messages_and_wait_for_processing(TestThreadedDestDriver *dd, gint n, StatsCounterItem *counter)
{
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT_NOACK;
  gchar buf[32];

  for (gint i = 0; i < n; i++)
    {
      msg = create_sample_message();

      g_snprintf(buf, sizeof(buf), "%d", i);
      log_msg_set_value(msg, LM_V_PID, buf, -1);

      log_pipe_queue(&dd->super.super.super.super, msg, &path_options);
    }
  _spin_for_counter_value(counter, n);
}

static void
_generate_message_and_wait_for_processing(TestThreadedDestDriver *dd, StatsCounterItem *counter)
{
  _generate_messages_and_wait_for_processing(dd, 1, counter);
}

TestThreadedDestDriver *dd;

static worker_insert_result_t
_insert_single_message_success(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  return WORKER_INSERT_RESULT_SUCCESS;
}

Test(logthrdestdrv, driver_can_be_instantiated_and_one_message_is_properly_processed)
{
  dd->super.worker.insert = _insert_single_message_success;

  _generate_message_and_wait_for_processing(dd, dd->super.written_messages);
  cr_assert(dd->insert_counter == 1);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 1);
  cr_assert(stats_counter_get(dd->super.written_messages) == 1);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == 2);
}

static worker_insert_result_t
_insert_single_message_drop(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  return WORKER_INSERT_RESULT_DROP;
}

Test(logthrdestdrv, message_drops_are_accounted_in_the_drop_counter_and_are_reported_properly)
{
  dd->super.worker.insert = _insert_single_message_drop;

  start_grabbing_messages();
  _generate_message_and_wait_for_processing(dd, dd->super.dropped_messages);
  cr_assert(dd->insert_counter == 1);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 1);
  cr_assert(stats_counter_get(dd->super.written_messages) == 0);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 1);
  cr_assert(dd->super.seq_num == 2);
  assert_grabbed_log_contains("dropped while sending");
}

static worker_insert_result_t
_insert_single_message_connection_failure(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  if (self->insert_counter++ < 10)
    return WORKER_INSERT_RESULT_NOT_CONNECTED;
  return WORKER_INSERT_RESULT_SUCCESS;
}

Test(logthrdestdrv, connection_failure_is_considered_an_error_and_retried_indefinitely)
{
  dd->super.worker.insert = _insert_single_message_connection_failure;
  dd->super.time_reopen = 0;

  start_grabbing_messages();
  _generate_message_and_wait_for_processing(dd, dd->super.written_messages);
  cr_assert(dd->insert_counter == 11);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 1);
  cr_assert(stats_counter_get(dd->super.written_messages) == 1);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(dd->super.seq_num == 12);
  assert_grabbed_log_contains("Server disconnected");
}

static worker_insert_result_t
_insert_single_message_error_until_drop(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  return WORKER_INSERT_RESULT_ERROR;
}

Test(logthrdestdrv, error_result_retries_sending_retry_max_times_and_then_drops)
{
  dd->super.worker.insert = _insert_single_message_error_until_drop;
  dd->super.time_reopen = 0;
  dd->super.retries_max = 5;

  start_grabbing_messages();
  _generate_message_and_wait_for_processing(dd, dd->super.dropped_messages);
  cr_assert(dd->insert_counter == 5);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 1);
  cr_assert(stats_counter_get(dd->super.written_messages) == 0);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 1);
  cr_assert(dd->super.seq_num == 6);
  assert_grabbed_log_contains("Error occurred while");
  assert_grabbed_log_contains("Multiple failures while sending");
}

static worker_insert_result_t
_insert_single_message_error_until_successful(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  if (self->insert_counter++ < 4)
    return WORKER_INSERT_RESULT_ERROR;
  return WORKER_INSERT_RESULT_SUCCESS;
}

Test(logthrdestdrv, error_result_retries_sending_retry_max_times_and_then_accepts)
{
  dd->super.worker.insert = _insert_single_message_error_until_successful;
  dd->super.time_reopen = 0;
  dd->super.retries_max = 5;

  start_grabbing_messages();
  _generate_message_and_wait_for_processing(dd, dd->super.written_messages);
  cr_assert(dd->insert_counter == 5);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 1);
  cr_assert(stats_counter_get(dd->super.written_messages) == 1);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(dd->super.seq_num == 6);
  assert_grabbed_log_contains("Error occurred while");
}

static worker_insert_result_t
_insert_batched_message_success(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  if (self->super.batch_size < 5)
    return WORKER_INSERT_RESULT_QUEUED;

  self->flush_size += self->super.batch_size;
  return WORKER_INSERT_RESULT_SUCCESS;
}

static worker_insert_result_t
_flush_batched_message_success(LogThreadedDestDriver *s)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->flush_counter++;
  self->flush_size += self->super.batch_size;
  return WORKER_INSERT_RESULT_SUCCESS;
}

Test(logthrdestdrv, batched_set_of_messages_are_successfully_delivered)
{
  dd->super.worker.insert = _insert_batched_message_success;
  dd->super.worker.flush = _flush_batched_message_success;

  _generate_messages_and_wait_for_processing(dd, 10, dd->super.written_messages);
  cr_assert(dd->insert_counter == 10);
  cr_assert(dd->flush_size == 10);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 10);
  cr_assert(stats_counter_get(dd->super.written_messages) == 10);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == 11, "%d", dd->super.seq_num);
}

static worker_insert_result_t
_insert_batched_message_drop(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  if (self->super.batch_size < 5)
    return WORKER_INSERT_RESULT_QUEUED;

  self->flush_size += self->super.batch_size;
  return WORKER_INSERT_RESULT_DROP;
}

static worker_insert_result_t
_flush_batched_message_drop(LogThreadedDestDriver *s)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->flush_counter++;
  self->flush_size += self->super.batch_size;
  return WORKER_INSERT_RESULT_DROP;
}

Test(logthrdestdrv, batched_set_of_messages_are_dropped_as_a_whole)
{
  dd->super.worker.insert = _insert_batched_message_drop;
  dd->super.worker.flush = _flush_batched_message_drop;
  dd->super.time_reopen = 0;

  start_grabbing_messages();
  _generate_messages_and_wait_for_processing(dd, 10, dd->super.dropped_messages);
  cr_assert(dd->insert_counter == 10);
  cr_assert(dd->flush_size == 10);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 10);
  cr_assert(stats_counter_get(dd->super.written_messages) == 0);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 10);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == 11, "%d", dd->super.seq_num);
  assert_grabbed_log_contains("dropped while sending message");
}

static inline void
_assert_batch_size_remains_the_same_accross_retries(TestThreadedDestDriver *self)
{
  if (self->super.retries_counter > 0)
    cr_assert(self->super.batch_size == self->prev_flush_size);
  else
    self->prev_flush_size = self->super.batch_size;
}

static worker_insert_result_t
_insert_batched_message_error_drop(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  if (self->super.batch_size < 5)
    return WORKER_INSERT_RESULT_QUEUED;

  self->flush_size += self->super.batch_size;
  _assert_batch_size_remains_the_same_accross_retries(self);
  return WORKER_INSERT_RESULT_ERROR;
}

static worker_insert_result_t
_flush_batched_message_error_drop(LogThreadedDestDriver *s)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->flush_size += self->super.batch_size;
  _assert_batch_size_remains_the_same_accross_retries(self);
  return WORKER_INSERT_RESULT_ERROR;
}

Test(logthrdestdrv, when_batched_set_of_messages_result_in_error_the_entire_batch_is_attempted_again_retry_count_times_and_then_dropped)
{
  dd->super.worker.insert = _insert_batched_message_error_drop;
  dd->super.worker.flush = _flush_batched_message_error_drop;
  dd->super.time_reopen = 0;
  dd->super.retries_max = 5;

  start_grabbing_messages();
  _generate_messages_and_wait_for_processing(dd, 10, dd->super.dropped_messages);

  cr_assert(dd->insert_counter == dd->super.retries_max * 10,
            "not all messages were attempted %d times, insert_counter=%d", dd->super.retries_max, dd->insert_counter);
  cr_assert(dd->flush_size == dd->super.retries_max * 10,
            "not all messages were flushed %d times, flush_size=%d", dd->super.retries_max, dd->flush_size);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 10);
  cr_assert(stats_counter_get(dd->super.written_messages) == 0);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 10);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == dd->super.retries_max * 10 + 1, "%d", dd->super.seq_num);
  assert_grabbed_log_contains("Error occurred while");
  assert_grabbed_log_contains("Multiple failures while sending");
}

/*
 * This testcase would try every message twice and fail and then be
 * successful at the third attempt.
 */

#define FAILING_ATTEMPTS_DROP 2

static inline worker_insert_result_t
_inject_error_a_few_times(TestThreadedDestDriver *self)
{
  if (self->super.retries_counter >= FAILING_ATTEMPTS_DROP)
    return WORKER_INSERT_RESULT_SUCCESS;
  else
    return WORKER_INSERT_RESULT_ERROR;
}

static worker_insert_result_t
_insert_batched_message_error_success(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  if (self->super.batch_size < 5)
    return WORKER_INSERT_RESULT_QUEUED;

  self->flush_size += self->super.batch_size;
  _assert_batch_size_remains_the_same_accross_retries(self);
  return _inject_error_a_few_times(self);
}

static worker_insert_result_t
_flush_batched_message_error_success(LogThreadedDestDriver *s)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->flush_size += self->super.batch_size;
  _assert_batch_size_remains_the_same_accross_retries(self);
  return _inject_error_a_few_times(self);
}

Test(logthrdestdrv, when_batched_set_of_messages_result_in_error_the_entire_batch_is_attempted_again_and_then_successfully_delivered)
{
  gint total_attempts = FAILING_ATTEMPTS_DROP + 1;

  dd->super.worker.insert = _insert_batched_message_error_success;
  dd->super.worker.flush = _flush_batched_message_error_success;
  dd->super.time_reopen = 0;
  dd->super.retries_max = 5;

  start_grabbing_messages();
  _generate_messages_and_wait_for_processing(dd, 10, dd->super.written_messages);

  cr_assert(dd->insert_counter == total_attempts * 10,
            "not all messages were attempted %d times, insert_counter=%d", total_attempts, dd->insert_counter);
  cr_assert(dd->flush_size == total_attempts * 10,
            "not all messages were flushed %d times, flush_size=%d", total_attempts, dd->flush_size);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 10);
  cr_assert(stats_counter_get(dd->super.written_messages) == 10);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == total_attempts * 10 + 1, "%d", dd->super.seq_num);
  assert_grabbed_log_contains("Error occurred while");
}

/*
 * This testcase would try every message twice and fail and then be
 * successful at the third attempt.
 */

#define FAILING_ATTEMPTS_NOTCONN 20

static inline worker_insert_result_t
_inject_not_connected_a_few_times(TestThreadedDestDriver *self)
{
  if (self->failure_counter++ >= FAILING_ATTEMPTS_NOTCONN)
    {
      self->failure_counter = 0;
      return WORKER_INSERT_RESULT_SUCCESS;
    }
  else
    return WORKER_INSERT_RESULT_NOT_CONNECTED;
}

static worker_insert_result_t
_insert_batched_message_not_connected(LogThreadedDestDriver *s, LogMessage *msg)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->insert_counter++;
  if (self->super.batch_size < 5)
    return WORKER_INSERT_RESULT_QUEUED;

  self->flush_size += self->super.batch_size;
  _assert_batch_size_remains_the_same_accross_retries(self);
  return _inject_not_connected_a_few_times(self);
}

static worker_insert_result_t
_flush_batched_message_not_connected(LogThreadedDestDriver *s)
{
  TestThreadedDestDriver *self = (TestThreadedDestDriver *) s;

  self->flush_size += self->super.batch_size;
  _assert_batch_size_remains_the_same_accross_retries(self);
  return _inject_not_connected_a_few_times(self);
}

Test(logthrdestdrv, when_batched_set_of_messages_result_in_not_connected_the_entire_batch_is_attempted_again_and_then_successfully_delivered)
{
  gint total_attempts = FAILING_ATTEMPTS_NOTCONN + 1;

  dd->super.worker.insert = _insert_batched_message_not_connected;
  dd->super.worker.flush = _flush_batched_message_not_connected;
  dd->super.time_reopen = 0;
  dd->super.retries_max = 5;

  start_grabbing_messages();
  _generate_messages_and_wait_for_processing(dd, 10, dd->super.written_messages);

  cr_assert(dd->insert_counter == total_attempts * 10,
            "not all messages were attempted %d times, insert_counter=%d", total_attempts, dd->insert_counter);
  cr_assert(dd->flush_size == total_attempts * 10,
            "not all messages were flushed %d times, flush_size=%d", total_attempts, dd->flush_size);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 10);
  cr_assert(stats_counter_get(dd->super.written_messages) == 10);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == total_attempts * 10 + 1, "%d", dd->super.seq_num);
  assert_grabbed_log_contains("Server disconnected");
}

Test(logthrdestdrv, throttle_is_applied_to_delivery_and_causes_flush_to_be_called_more_often)
{
  /* 3 messages per second, we need to set this explicitly on the queue as it has already been initialized */
  log_queue_set_throttle(dd->super.worker.queue, 3);
  dd->super.worker.insert = _insert_batched_message_success;
  dd->super.worker.flush = _flush_batched_message_success;

  start_stopwatch();
  _generate_messages_and_wait_for_processing(dd, 20, dd->super.written_messages);
  guint64 time_msec = stop_stopwatch_and_get_result();

  /* NOTE: initially we send a bucket worth of messages, and then pace out
   * the remaining 6 buckets 1sec apart */

  cr_assert(time_msec > 5000000);
  cr_assert(dd->insert_counter == 20);
  cr_assert(dd->flush_size == 20);
  cr_assert(dd->flush_counter > 3);

  cr_assert(stats_counter_get(dd->super.processed_messages) == 20);
  cr_assert(stats_counter_get(dd->super.written_messages) == 20);
  cr_assert(stats_counter_get(dd->super.dropped_messages) == 0);
  cr_assert(stats_counter_get(dd->super.memory_usage) == 0);
  cr_assert(dd->super.seq_num == 21, "%d", dd->super.seq_num);
}

MainLoop *main_loop;
MainLoopOptions main_loop_options = {0};

static void
setup(void)
{
  app_startup();

  main_loop = main_loop_get_instance();
  main_loop_init(main_loop, &main_loop_options);
  dd = test_threaded_dd_new(main_loop_get_current_config(main_loop));

  cr_assert(log_pipe_init(&dd->super.super.super.super));
}

static void
teardown(void)
{
  main_loop_sync_worker_startup_and_teardown();
  log_pipe_deinit(&dd->super.super.super.super);
  log_pipe_unref(&dd->super.super.super.super);
  main_loop_deinit(main_loop);
  app_shutdown();
}

TestSuite(logthrdestdrv, .init = setup, .fini = teardown);
