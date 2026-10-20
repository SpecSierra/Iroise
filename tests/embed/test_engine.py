"""The engine and our front end for it (ui/SFOS), driven over the embedding protocol as the interface drives them.

These test what the engine's own tests cannot: what our front end makes of a finger (taps, drags, flicks), and what
it tells the interface (loads, history, dialogs, downloads, fullscreen, crashes). Run with scripts/test-front-end.sh.
"""

import json
import os
import signal
import time
import unittest

from embed import Engine, Server, page


class EngineTest(unittest.TestCase):
    def setUp(self):
        self.engine = Engine()

    def tearDown(self):
        self.engine.close()
        # Each test's engine log and messages, for the CI's results.
        if os.environ.get('PACIFIC_TEST_LOGS'):
            os.makedirs(os.environ['PACIFIC_TEST_LOGS'], exist_ok=True)
            with open(os.path.join(os.environ['PACIFIC_TEST_LOGS'], self.id() + '.log'), 'w') as log:
                log.write(self.engine.log())
                log.write('\n--- messages ---\n' + '\n'.join(map(repr, self.engine.messages)))
        self.engine.remove()

    # Device pixels: twice CSS pixels (Engine.SCALE).
    middle = (Engine.WIDTH / 2, Engine.HEIGHT / 2)


class Loading(EngineTest):
    def test_a_load_reports_its_url_title_and_end(self):
        self.engine.load(page('one.html'))
        self.engine.wait_for('TITLE', lambda message: message.text == 'One')
        self.assertEqual(self.engine.eval('document.title'), 'One')

    def test_history_goes_back_and_forward(self):
        self.engine.load(page('one.html'))
        self.engine.load(page('two.html'))
        self.engine.wait_for('HISTORY', lambda message: message.a == 1 and message.b == 0)
        # NB: A traversal tells the history before the URL.
        self.engine.send('GO_BACK')
        self.engine.wait_for('HISTORY', lambda message: message.a == 0 and message.b == 1)
        self.engine.wait_for('URL', lambda message: message.text.endswith('one.html'))
        self.engine.send('GO_FORWARD')
        self.engine.wait_for('HISTORY', lambda message: message.a == 1 and message.b == 0)
        self.engine.wait_for('URL', lambda message: message.text.endswith('two.html'))

    def test_a_page_message_reaches_the_interface(self):
        self.engine.load(page('message.html'))
        self.engine.wait_for('PAGE_MESSAGE', lambda message: message.text == 'hello from the page')


class Taps(EngineTest):
    def test_a_tap_follows_a_link(self):
        self.engine.load(page('link.html'))
        self.engine.tap(*self.middle)
        self.engine.wait_for('URL', lambda message: message.text.endswith('two.html'))

    def test_a_tap_is_touch_then_mouse_events_once(self):
        self.engine.load(page('events.html'))
        self.engine.tap(*self.middle)
        self.assertEqual(self.engine.eval('order'), [
            'pointerdown:touch', 'touchstart', 'pointerup:touch', 'touchend', 'mousedown', 'mouseup', 'click'])

    def test_a_cancelled_touchend_is_no_click(self):
        self.engine.load(page('events.html#cancel-touchend'))
        self.engine.tap(*self.middle)
        self.assertEqual(self.engine.eval('order'), ['pointerdown:touch', 'touchstart', 'pointerup:touch', 'touchend'])

    def test_a_tap_on_a_text_field_brings_the_keyboard_and_takes_text(self):
        self.engine.load(page('input.html'))
        self.engine.tap(Engine.WIDTH / 2, Engine.HEIGHT / 4)
        self.engine.wait_for('TEXT_INPUT', lambda message: message.a == 1)
        self.engine.send('COMMIT_TEXT', text='Pacific')
        self.engine.pump(0.3)
        self.assertEqual(self.engine.eval('field.value'), 'Pacific')


class Scrolling(EngineTest):
    def scroll_y(self):
        return self.engine.eval('scrollY')

    def test_a_drag_scrolls_with_the_finger(self):
        self.engine.load(page('tall.html'))
        self.engine.drag((540, 1600), (540, 600), seconds=0.6, then=1)
        # 1000 device pixels are 500 CSS pixels; what a fling adds on top of that is not the point here.
        self.assertGreaterEqual(self.scroll_y(), 400)
        self.assertGreater(self.engine.eval('pointers.cancel'), 0, 'a scroll cancels the touch\'s pointer')

    def test_a_flick_goes_on_after_the_finger_lifts(self):
        self.engine.load(page('tall.html'))
        self.engine.drag((540, 1600), (540, 1000), seconds=0.08, steps=8, then=0)
        at_lift = self.scroll_y()
        self.engine.pump(1.5)
        self.assertGreater(self.scroll_y(), at_lift + 50, f'it stopped at {at_lift}')

    def test_touch_action_none_keeps_the_finger_the_pages(self):
        self.engine.load(page('tall.html#touch-action-none'))
        self.engine.drag((540, 1600), (540, 600), seconds=0.6)
        self.assertEqual(self.scroll_y(), 0)
        self.assertEqual(self.engine.eval('pointers.cancel'), 0)
        self.assertGreater(self.engine.eval('pointers.move'), 5)

    def test_a_cancelled_touchmove_keeps_the_finger_the_pages(self):
        self.engine.load(page('tall.html#touchmove-cancelled'))
        self.engine.drag((540, 1600), (540, 600), seconds=0.6, then=1.5)
        self.assertEqual(self.scroll_y(), 0)

    def test_a_flick_the_page_cancelled_does_not_fling(self):
        # The compositor flings by the speed of a gesture's steps, whether or not the page let them scroll.
        self.engine.load(page('tall.html#touchmove-cancelled-on-an-element'))
        self.engine.drag((540, 1600), (540, 1000), seconds=0.1, steps=8, then=1.5)
        self.assertEqual(self.scroll_y(), 0)
        self.assertEqual(self.engine.eval('pointers.cancel'), 0)


class Dialogs(EngineTest):
    def test_confirm_asks_the_interface_and_takes_its_answer(self):
        self.engine.load(page('dialog.html'))
        self.engine.tap(*self.middle)
        dialog = self.engine.wait_for('DIALOG')
        self.assertEqual(json.loads(dialog.text)['message'], 'Proceed?')
        self.engine.send('DIALOG_CLOSED', a=1, b=dialog.a)
        self.engine.pump(0.3)
        self.assertIs(self.engine.eval('window.answer'), True)


class Fullscreen(EngineTest):
    def test_fullscreen_is_reported_and_left_from_the_interface(self):
        self.engine.load(page('fullscreen.html'))
        self.engine.tap(*self.middle)
        self.engine.wait_for('FULLSCREEN', lambda message: message.a == 1)
        self.engine.send('EXIT_FULLSCREEN')
        self.engine.wait_for('FULLSCREEN', lambda message: message.a == 0)


class Downloads(EngineTest):
    def setUp(self):
        super().setUp()
        self.server = Server()

    def tearDown(self):
        self.server.close()
        super().tearDown()

    def test_a_tapped_download_link_saves_the_file(self):
        self.engine.load(self.server.url('download.html'))
        self.engine.tap(*self.middle)
        done = self.engine.wait_for('DOWNLOAD', lambda message: message.b != 0, timeout=30)
        self.assertEqual(done.b, 1, f'download ended as {done}')
        path = json.loads(done.text)['path']
        with open(path, 'rb') as file, open(os.path.join(os.path.dirname(__file__), 'pages', 'file.bin'), 'rb') as original:
            self.assertEqual(file.read(), original.read())


class Crashes(EngineTest):
    def test_a_crashed_page_is_reported_and_loads_again(self):
        self.engine.load(page('one.html'))
        pages = self.engine.page_processes()
        self.assertTrue(pages, 'no WebContent process found')
        for pid in pages:
            os.kill(pid, signal.SIGKILL)
        self.engine.wait_for('CRASHED')
        self.engine.load(page('two.html'))
        self.assertEqual(self.engine.eval('document.title'), 'Two')


if __name__ == '__main__':
    unittest.main()
