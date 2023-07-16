# frozen_string_literal: true

require "test_helper"

class TestTimeCollector < Minitest::Test
  def bar
    sleep 0.1
  end

  def foo
    bar
    1.times do
      bar
    end
  end

  def test_time_collector
    collector = Vernier::Collector.new(:wall)
    collector.start
    foo
    result = collector.stop

    assert_valid_result result
    assert_includes (380..420), result.samples.size

    significant_stacks = result.samples.tally.select { |k,v| v > 10 }
    assert_equal 2, significant_stacks.size
    assert significant_stacks.sum(&:last) > 350
  end

  def test_threads
    collector = Vernier::Collector.new(:wall)
    th1 = Thread.new { foo }
    th2 = Thread.new { foo }
    collector.start
    foo
    th1.join
    th2.join
    result = collector.stop
  end
end