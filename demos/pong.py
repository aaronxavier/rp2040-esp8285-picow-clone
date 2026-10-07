#!/usr/bin/env python3
"""Pong on the 8x8 matrix, driven only through the HTTP API (/img).

    python3 pong.py [host]       # default host: pico-esp.local, Ctrl+C to stop
    python3 pong.py --selftest   # run the game logic offline, no network
"""
import random, socket, sys, time, urllib.request

W = H = 8
PADDLE = 3
LEFT_C, RIGHT_C, BALL_C, TRAIL_C = (0, 40, 120), (120, 20, 0), (110, 110, 110), (20, 20, 20)
WIN = 5
MISS_CHANCE = 0.18  # chance the AI hesitates for a tick; 0 = never loses


class Pong:
    def __init__(self):
        self.score = [0, 0]
        self.pad = [2, 3]  # top y of left/right paddle
        self.serve(random.choice((-1, 1)))

    def serve(self, dx):
        self.bx, self.by = 3 if dx > 0 else 4, random.randrange(2, 6)
        self.dx, self.dy = dx, random.choice((-1, 1))
        self.trail = None
        self.hits = 0

    def ai(self, side):
        moving_toward = (self.dx < 0) == (side == 0)
        target = self.by - PADDLE // 2 if moving_toward else (H - PADDLE) // 2
        if random.random() < MISS_CHANCE:
            return
        p = self.pad[side]
        self.pad[side] = max(0, min(H - PADDLE, p + (target > p) - (target < p)))

    def step(self):
        """Advance one tick. Returns the scoring side (0/1) or None."""
        self.ai(0)
        self.ai(1)
        self.trail = (self.bx, self.by)
        nx, ny = self.bx + self.dx, self.by + self.dy
        if not 0 <= ny < H:  # bounce off top/bottom
            self.dy = -self.dy
            ny = self.by + self.dy
        if nx in (0, W - 1):
            side = 0 if nx == 0 else 1
            off = ny - self.pad[side]
            if 0 <= off < PADDLE:  # hit: edge of paddle sends it off at an angle
                self.dx = -self.dx
                self.dy = -1 if off == 0 else 1 if off == PADDLE - 1 else self.dy
                self.hits += 1
                nx, ny = self.bx + self.dx, self.by + self.dy
                if not 0 <= ny < H:
                    self.dy = -self.dy
                    ny = self.by + self.dy
            else:  # miss: ball goes into the gutter, other side scores
                self.bx, self.by = nx, ny
                return 1 - side
        self.bx, self.by = nx, ny
        assert 0 < self.bx < W - 1 and 0 <= self.by < H, (self.bx, self.by)
        return None

    def frame(self):
        px = [[(0, 0, 0)] * W for _ in range(H)]
        for y in range(PADDLE):
            px[self.pad[0] + y][0] = LEFT_C
            px[self.pad[1] + y][W - 1] = RIGHT_C
        if self.trail:
            px[self.trail[1]][self.trail[0]] = TRAIL_C
        px[self.by][self.bx] = BALL_C
        return px


def score_frame(score):
    """Left score as blue dots on the left half, right score red on the right, bottom up."""
    px = [[(0, 0, 0)] * W for _ in range(H)]
    for side, c, x in ((0, LEFT_C, 1), (1, RIGHT_C, W - 3)):
        for i in range(score[side]):
            px[H - 1 - i][x] = px[H - 1 - i][x + 1] = c
    return px


def fill(c):
    return [[c] * W for _ in range(H)]


def send(host, px):
    d = ''.join('%02x%02x%02x' % c for row in px for c in row)
    try:
        urllib.request.urlopen(f'http://{host}/img?d={d}', timeout=1).read()
    except OSError:
        pass  # dropped frame; the next one will catch up


def selftest():
    random.seed(1)
    g, points = Pong(), 0
    for _ in range(100000):
        if g.step() is not None:
            points += 1
            g.serve(random.choice((-1, 1)))
    assert 100 < points < 50000, points  # both rallies and points happen
    assert len(score_frame([5, 5])) == H and all(len(r) == W for r in g.frame())
    print('selftest ok,', points, 'points in 100000 ticks')


def main(host):
    host = socket.gethostbyname(host)  # resolve mDNS once, not per frame
    g = Pong()
    while True:
        t = time.monotonic()
        scorer = g.step()
        send(host, g.frame())
        if scorer is not None:
            g.score[scorer] += 1
            print('score %d : %d' % tuple(g.score))
            time.sleep(0.4)
            send(host, score_frame(g.score))
            time.sleep(1.2)
            if WIN in g.score:
                for _ in range(3):  # flash the winner's color
                    send(host, fill((LEFT_C, RIGHT_C)[scorer]))
                    time.sleep(0.25)
                    send(host, fill((0, 0, 0)))
                    time.sleep(0.25)
                g.score = [0, 0]
            g.serve(1 if scorer == 0 else -1)  # serve toward the side that just lost the point
            continue
        tick = max(0.07, 0.15 - 0.01 * g.hits)  # rallies speed up
        time.sleep(max(0, tick - (time.monotonic() - t)))


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        selftest()
    else:
        try:
            main(sys.argv[1] if len(sys.argv) > 1 else 'pico-esp.local')
        except KeyboardInterrupt:
            pass
