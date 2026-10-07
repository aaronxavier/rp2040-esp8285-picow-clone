#!/usr/bin/env python3
"""Regenerate wiring.svg:  pip install schemdraw && python3 wiring.py"""
import schemdraw
import schemdraw.elements as elm

schemdraw.config(fontsize=11, lw=1.2, bgcolor='white')  # readable in GitHub dark mode too

with schemdraw.Drawing(file='wiring.svg', show=False) as d:
    rp = elm.Ic(pins=[
        elm.IcPin(name='GP0 / UART0 TX', pin='1', side='left', slot='1/2'),
        elm.IcPin(name='GP1 / UART0 RX', pin='2', side='left', slot='2/2'),
        elm.IcPin(name='GP22', pin='29', side='right', slot='3/3'),
        elm.IcPin(name='VSYS', pin='39', side='right', slot='2/3'),
        elm.IcPin(name='GND', pin='38', side='right', slot='1/3'),
    ], size=(4.6, 3.5), pinspacing=1, edgepadH=0.6).label('RP2040', loc='top', fontsize=13)

    # ESP8285 sits left of the RP2040, wired on the board itself
    d.push()
    d.move_from(rp.pin1, dx=-4.0, dy=0)
    esp = elm.Ic(pins=[
        elm.IcPin(name='RX', side='right', slot='1/2'),
        elm.IcPin(name='TX', side='right', slot='2/2'),
    ], size=(1.6, 2.0), pinspacing=1).label('ESP8285', loc='top', fontsize=13).anchor('RX')
    d.pop()
    elm.Wire('-').at(esp.RX).to(rp.pin1).color('#888')
    elm.Wire('-').at(esp.TX).to(rp.pin2).color('#888')
    elm.Label().at(((esp.RX[0] + rp.pin1[0]) / 2, esp.RX[1] - 0.6)).label('on-board UART0\n115200 baud', fontsize=9, color='#888')
    elm.Label().at((esp.center[0] + 0.7, esp.RX[1] - 1.3)).label('flash mode: hold the\nbutton by the WiFi chip', fontsize=9, color='#888')

    elm.EncircleBox([esp, rp], padx=0.4, pady=0.5).linestyle('--').color('#888').label(
        'TZT "Pico W" clone board', loc='top', fontsize=11)

    # 8x8 WS2812 matrix to the right
    d.push()
    d.move_from(rp.pin29, dx=4.5, dy=0)
    mx = elm.Ic(pins=[
        elm.IcPin(name='DIN', side='left', slot='3/3'),
        elm.IcPin(name='5V', side='left', slot='2/3'),
        elm.IcPin(name='GND', side='left', slot='1/3'),
        elm.IcPin(name='DOUT', side='right', slot='3/3'),
    ], size=(2.8, 3.5), pinspacing=1, edgepadH=0.6).label('WS2812 8x8 matrix', loc='top', fontsize=13).anchor('DIN')
    d.pop()
    elm.Wire('-').at(rp.pin29).to(mx.DIN).color('#2a7')
    elm.Wire('-').at(rp.pin39).to(mx['5V']).color('#d33')
    elm.Wire('-').at(rp.pin38).to(mx.GND).color('#333')
    elm.Label().at((mx.center[0], mx.GND[1] - 1.6)).label('DOUT: leave unconnected\n(data goes into DIN!)', fontsize=9, color='#888')
    elm.Label().at(((rp.pin39[0] + mx['5V'][0]) / 2, rp.pin39[1] - 0.35)).label('max ~300 mA, see pico/power.h', fontsize=9, color='#888')
