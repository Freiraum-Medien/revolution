#!/usr/bin/env python3
# testdaten.py – Testdaten in die vier Testkonten einspielen (#344)
#
# Versand: SMTP über Camel mit den Transport-Quellen von privat@ und geops@.
# Ablage: Camel-Stores der vier Konten (imapx bzw. microsoft365), konfiguriert
# aus der ESourceRegistry wie in Evolution (e_source_camel_configure_service).
# Zugangsdaten nur zur Laufzeit aus GOA, nie ausgegeben, nie gespeichert.
#
# Aufruf:
#   tests/testdaten.py            einspielen (fehlende Teile ergänzen)
#   tests/testdaten.py --dry-run  nur anzeigen, was fehlt
#   tests/testdaten.py --pruefen  nur lesend belegen, was vorhanden ist
#   tests/testdaten.py --ordner   nur lesend: Ordnerliste aller Mail-Konten aus GOA (#348),
#                                 auch der nur lesend freigegebenen; Vergleich mit dem Log
#                                 der App (G_MESSAGES_DEBUG=revolution, „Ordner: UID Name“)
#   tests/testdaten.py --zaehler [LOG]  #350: nur lesend, unabhängig von Camel: ungelesene
#                                 Nachrichten je Ordner aller Mail-Konten aus GOA, IMAP per STATUS
#                                 (kein SELECT/EXAMINE, \\Recent bleibt), microsoft365 per Graph
#                                 (GET); Zeilen „ZAEHLER <UID> <Ordner> <N>“. Mit LOG (App-Log,
#                                 G_MESSAGES_DEBUG=revolution) Vergleich mit dem letzten
#                                 „Ungelesen: <UID> <Ordner> <N>“ je Ordner, Exit 1 bei Abweichung
#   tests/testdaten.py --last N   #352: N Lastmails per IMAP-APPEND (kein Versand) in den Ordner
#                                 „Revolution-Last“ von privat@ (fehlende ergänzen); letzte
#                                 Zeile „LAST <UID>/<voller Ordnername> <Anzahl>“; #350: mit
#                                 --last-ordner NAME in einen anderen Ordner (Abbruch-Test Zähler)
#   tests/testdaten.py --flags KONTO
#                                 #353, nur lesend, unabhängig von Camel: Flags der Mails im Ordner
#                                 Revolution-Test direkt vom Server (IMAP EXAMINE + UID FETCH FLAGS
#                                 bzw. Graph GET isRead/importance/flag); Zeilen „ORDNER <UID>/<Name>“
#                                 und „FLAGS <gelesen 0|1> <markiert 0|1> <Betreff>“
#   tests/testdaten.py --server KONTO AKTION
#                                 #353, schreibend nur in Testkonten: serverseitig an der Einladung
#                                 in Revolution-Test gelesen|ungelesen|markiert|unmarkiert (IMAP UID
#                                 STORE bzw. Graph PATCH), neu (IMAP APPEND einer Live-Mail) oder
#                                 weg (Live-Mails löschen: UID STORE \Deleted + EXPUNGE); #414: mit
#                                 --warte DATEI erst anmelden, „BEREIT“ ausgeben, dann ablegen,
#                                 sobald DATEI existiert, und „ABGELEGT <Uhrzeit>“ ausgeben
#   tests/testdaten.py --ungelesen KONTO ORDNER [--soll DATEI]
#                                 #353: Zeile „UNGELESEN <Anzahl> <UID …>“ (IMAP EXAMINE + UID SEARCH
#                                 UNSEEN, nur lesend); mit --soll (Datei mit einer solchen Zeile)
#                                 stellt es genau diesen Stand per UID STORE ±\Seen her (Testkonten)
#   tests/testdaten.py --anzahl UID ORDNER
#                                 #410: nur lesend, unabhängig von Camel, jedes Konto aus GOA: Anzahl
#                                 der Nachrichten eines IMAP-Ordners (EXAMINE, UID SEARCH UNDELETED
#                                 ohne Junk); Zeile „ANZAHL <UID>/<Ordner> <sichtbar> EXISTS <n>“;
#                                 #414: microsoft365 per Graph GET (totalItemCount, beide Zahlen gleich)
#   tests/testdaten.py --anzeige  #354: nur die Anzeige-Mails (HTML, Text ISO-8859-1, Inline-Bild
#                                 cid:, Remote-Inhalte; #355: zweite Remote-Mail und Remote-Mail eines
#                                 anderen Absenders) per APPEND in den Posteingang der vier
#                                 Testkonten (fehlende ergänzen, kein Versand); im Standardlauf
#                                 enthalten
#   tests/testdaten.py --anhang   #356: Anhang-Mail (PDF, PNG, ZIP, Text mit „../“ im Dateinamen,
#                                 angehängte Nachricht) per APPEND in den Posteingang von geops@,
#                                 outlook.com und Gmail (nicht privat@: Quota); im Standardlauf
#                                 enthalten
#   tests/testdaten.py --anhang-teile KONTO ORDNER
#                                 #356, nur lesend, unabhängig von Camel: die Anhänge der Anhang-Mail
#                                 direkt vom Server (IMAP EXAMINE + UID FETCH BODY.PEEK[] bzw. Graph
#                                 GET …/attachments) nach ORDNER schreiben; je Anhang eine Zeile
#                                 „TEIL <sha256> <Bytes> <Dateiname>“, für Text zusätzlich „TEIL-TEXT …“
#                                 mit Zeilenenden CRLF → LF (so dekodiert Camel text/*)
#   tests/testdaten.py --kombiniert [LOG]
#                                 #351, nur lesend, unabhängig von Camel, jedes Konto aus GOA: je
#                                 Posteingang sichtbare und ungelesene Nachrichten (IMAP EXAMINE +
#                                 UID SEARCH UNDELETED ohne Junk, Graph GET); Zeilen „KOMBINIERT <UID>
#                                 <Adresse> <sichtbar> <ungelesen>“ und „SUMME …“. Mit LOG Vergleich mit
#                                 dem letzten „Favoriten: Anzahl inbox <UID> …“ je Konto, der Summe der
#                                 Liste und „Ungelesen: revolution-favoriten inbox <N>“; Exit 1 bei
#                                 Abweichung
#   tests/testdaten.py --live-flags KONTO
#                                 #351, nur lesend: Flags der Live-Mails (--server KONTO neu INBOX) im
#                                 Posteingang (IMAP EXAMINE + UID SEARCH SUBJECT + UID FETCH FLAGS);
#                                 Zeilen „FLAGS <gelesen 0|1> <markiert 0|1> <UID>“
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import base64
import ctypes
import datetime
import hashlib
import imaplib
import io
import json
import re
import email.header
import email.policy
import email.utils
import os
import urllib.parse
import urllib.request
import struct
import sys
import threading
import time
import zipfile
import zlib
from email.message import EmailMessage

import gi
gi.require_version('Camel', '1.2')
gi.require_version('EDataServer', '1.2')
gi.require_version('Goa', '1.0')
from gi.repository import GLib, Gio, Camel, EDataServer, Goa

PREFIX = '[Revolution-Test]'
TESTKONTEN = ('KONTO_IMAP_1@example.invalid', 'KONTO_IMAP_2@example.invalid',
              'KONTO_M365@example.invalid', 'KONTO_GOOGLE@example.invalid')
NUR_LESEND = ('NUR_LESEND_1@example.invalid', 'NUR_LESEND_2@example.invalid', 'NUR_LESEND_3@example.invalid')
ABSENDER = ('KONTO_IMAP_1@example.invalid', 'KONTO_IMAP_2@example.invalid')
ORDNER = 'Revolution-Test'
LAST_ORDNER = 'Revolution-Last'
LAST_KONTO = 'KONTO_IMAP_1@example.invalid'
UNTERORDNER = ORDNER + '/Archiv-Test'
REMOTE_BILD = 'https://upload.wikimedia.org/wikipedia/commons/a/a9/Example.jpg'
TIMEOUT = 90
WARTEN = 600
CACHE = os.path.join(GLib.get_user_cache_dir(), 'revolution', 'testdaten')


def nur_testkonten(adressen):
    """Harte Prüfung: jede Adresse, an die gesendet oder in die geschrieben wird."""
    for a in adressen:
        if a.lower() in NUR_LESEND or a.lower() not in TESTKONTEN:
            sys.exit('ABBRUCH: %s ist kein Testkonto' % a)


def cancellable():
    c = Gio.Cancellable()
    t = threading.Timer(TIMEOUT, c.cancel)
    t.daemon = True
    t.start()
    return c


# ---------------------------------------------------------------- Testdaten

def png_bytes():
    w = h = 16
    raw = b''.join(b'\x00' + b'\x35\x84\xe4' * w for _ in range(h))

    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data
                + struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def pdf_bytes():
    text = b'BT /F1 18 Tf 72 720 Td (Revolution-Test) Tj ET'
    objs = [b'<< /Type /Catalog /Pages 2 0 R >>',
            b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
            b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] '
            b'/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
            b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
            b'<< /Length %d >>\nstream\n%s\nendstream' % (len(text), text)]
    out = b'%PDF-1.4\n'
    offsets = []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += b'%d 0 obj\n%s\nendobj\n' % (i, o)
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
    out += b''.join(b'%010d 00000 n \n' % off for off in offsets)
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, xref)
    return out


def ics_text():
    att = ''.join('ATTENDEE;CN=%s;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:%s\r\n'
                  % (a, a) for a in TESTKONTEN if a != 'KONTO_IMAP_1@example.invalid')
    return ('BEGIN:VCALENDAR\r\nPRODID:-//Revolution//testdaten//DE\r\nVERSION:2.0\r\n'
            'METHOD:REQUEST\r\nBEGIN:VEVENT\r\nUID:revolution-test-einladung@example.invalid\r\n'
            'DTSTAMP:20261023T080000Z\r\nDTSTART:20261105T090000Z\r\nDTEND:20261105T100000Z\r\n'
            'SUMMARY:%s Abstimmung Testdaten\r\nLOCATION:Online\r\nSEQUENCE:0\r\n'
            'STATUS:CONFIRMED\r\nORGANIZER;CN=KONTO_IMAP_1@example.invalid:mailto:KONTO_IMAP_1@example.invalid\r\n'
            '%sEND:VEVENT\r\nEND:VCALENDAR\r\n' % (PREFIX, att))


def msgid(name):
    return '<revolution-test-%s@example.invalid>' % name


# Jede Mail: Schlüssel, Absender, Betreff, Bauanweisung, gelesen?
# Erkennung „schon vorhanden“ über (Betreff, Absender).
def baue(key):
    m = EmailMessage(policy=email.policy.SMTP)
    if key == 'text':
        m.set_content('Einfache Text-Mail. Bleibt ungelesen.\n')
    elif key == 'html':
        m.set_content('HTML-Mail mit Remote-Bild: %s\n' % REMOTE_BILD)
        m.add_alternative('<html><body><p>HTML-Mail mit <b>Remote-Bild</b>:</p>'
                          '<p><img src="%s" alt="Beispielbild" width="172" height="178"></p>'
                          '</body></html>\n' % REMOTE_BILD, subtype='html')
    elif key == 'anhang':
        m.set_content('Mail mit zwei Anhängen (PNG, PDF).\n')
        m.add_attachment(png_bytes(), maintype='image', subtype='png', filename='revolution-test.png')
        m.add_attachment(pdf_bytes(), maintype='application', subtype='pdf', filename='revolution-test.pdf')
    elif key.startswith('thread'):
        m.set_content('Thread-Mail %s von 3.\n' % key[-1])
    elif key == 'einladung':
        m.set_content('Einladung: Abstimmung Testdaten am 05.11.2026, 10:00 Uhr.\n')
        m.add_alternative(ics_text(), subtype='calendar', params={'method': 'REQUEST'})
    return m


MAILS = [
    ('text', 'KONTO_IMAP_1@example.invalid', PREFIX + ' Einfache Text-Mail', False),
    ('html', 'KONTO_IMAP_2@example.invalid', PREFIX + ' HTML mit Remote-Bild', True),
    ('anhang', 'KONTO_IMAP_1@example.invalid', PREFIX + ' Anhänge PNG und PDF', True),
    ('thread1', 'KONTO_IMAP_1@example.invalid', PREFIX + ' Projektplanung', True),
    ('thread2', 'KONTO_IMAP_2@example.invalid', PREFIX + ' Re: Projektplanung', True),
    ('thread3', 'KONTO_IMAP_1@example.invalid', PREFIX + ' Re: Projektplanung', True),
    ('einladung', 'KONTO_IMAP_1@example.invalid', PREFIX + ' Einladung: Abstimmung Testdaten', True),
]
KOPIEN = {'anhang': UNTERORDNER, 'einladung': ORDNER}

# #354: Mails für die Anzeige, per APPEND (kein Versand) in den Posteingang jedes Testkontos.
# Absender als Kopfzeile ein Testkonto mit Anzeigenamen. Erkennung wie MAILS über (Betreff, Absender).
ANZEIGE_ABSENDER = 'KONTO_IMAP_1@example.invalid'
ANZEIGE = [
    ('anzeige-html', PREFIX + ' Anzeige HTML: Jahresabrechnung'),
    ('anzeige-latin1', PREFIX + ' Anzeige Text ISO-8859-1: Grüße aus Köln'),
    ('anzeige-cid', PREFIX + ' Anzeige Inline-Bild (cid)'),
    ('anzeige-remote', PREFIX + ' Anzeige Remote-Inhalte (Tracking-Pixel)'),
    ('anzeige-dunkel', PREFIX + ' Anzeige HTML mit Dunkel-Fassung'),
    # #355: zweite Mail desselben Absenders und eine von einem anderen Absender, je mit Remote-Bild
    ('anzeige-remote2', PREFIX + ' Anzeige Remote zweite Mail'),
    ('anzeige-remote-fremd', PREFIX + ' Anzeige Remote anderer Absender'),
]
# #355: abweichender Absender je Schlüssel (sonst ANZEIGE_ABSENDER)
ANZEIGE_ANDERE = {'anzeige-remote-fremd': 'KONTO_IMAP_2@example.invalid'}
REMOTE_HOST = 'https://upload.wikimedia.org'


def anzeige_html():
    zeilen = ''.join('<tr><td style="padding:6px 0">%s</td><td style="padding:6px 0;text-align:right">%s</td></tr>'
                     % z for z in [('Abrechnungszeitraum', '01.01. – 31.12.2026'), ('Verbrauch Strom', '2.418 kWh'),
                                   ('Neuer Abschlag ab Januar', '72,00 € monatlich'),
                                   ('<b>Ihr Guthaben</b>', '<b style="color:#0b7fab">84,20 €</b>')])
    absatz = ('<p>Das Guthaben überweisen wir in den nächsten Tagen auf Ihr Konto. Den neuen Abschlag '
              'haben wir an Ihren Verbrauch angepasst. Fragen beantworten wir gern unter '
              '<a href="mailto:KONTO_IMAP_1@example.invalid">KONTO_IMAP_1@example.invalid</a>.</p>')
    return ('<!DOCTYPE html><html><head><meta charset="utf-8"><style>'
            'body{font-family:sans-serif;color:#1d1d1d;margin:0}'
            '.rahmen{max-width:640px;margin:0 auto;padding:32px 40px}'
            '.kopf{display:flex;justify-content:space-between;border-bottom:2px solid #0b7fab;padding-bottom:12px}'
            '.marke{color:#0b7fab;font-size:22px;font-weight:bold}'
            '.box{background:#eef5f9;border-radius:10px;padding:14px 22px;margin:20px 0}'
            '.knopf{display:inline-block;background:#0b7fab;color:#fff;padding:12px 24px;border-radius:4px;'
            'text-decoration:none;font-weight:bold}'
            '@media (prefers-color-scheme: dark){.box{background:#1d3340}}'
            '</style></head><body><div class="rahmen">'
            '<div class="kopf"><span class="marke">Stadtwerke Lindenau</span>'
            '<span style="color:#666;font-size:13px">Kundennummer 104 227 1</span></div>'
            '<h1>Ihre Jahresabrechnung 2026 ist da</h1>'
            '<p>Sehr geehrte Frau Vogt,</p><p>vielen Dank, dass Sie Ihre Energie von uns beziehen. Ihre '
            '<b>Jahresabrechnung für 2026</b> steht ab sofort bereit. Sie haben weniger verbraucht als im '
            'Vorjahr und erhalten ein <b>Guthaben</b>. Umlaute und Zeichen: äöü ÄÖÜ ß € „Anführung“ – Strich.</p>'
            '<table class="box" width="100%%">%s</table>%s'
            '<p><a class="knopf" href="https://www.gnome.org/">Zur Rechnung im Kundenportal</a></p>'
            '%s<p>Freundliche Grüße<br>Ihr Kundenservice der Stadtwerke Lindenau</p>'
            '<hr><p style="color:#666;font-size:12px">Testmail für Revolution #354 (HTML, UTF-8, ohne '
            'Remote-Inhalte). Ende der Nachricht.</p></div></body></html>\n'
            % (zeilen, absatz * 3, absatz * 3))


def anzeige_dunkel_html():
    """#354 Nacharbeit: Mail mit vollständiger eigener Dunkel-Fassung (meta color-scheme „light dark“,
    Medienabfrage setzt Hintergrund und Schrift) und einem Link mit ping-Attribut (Hyperlink-Auditing)."""
    return ('<!DOCTYPE html><html><head><meta charset="utf-8">'
            '<meta name="color-scheme" content="light dark"><meta name="supported-color-schemes" content="light dark">'
            '<style>body{font-family:sans-serif;margin:0;background:#ffffff;color:#1d1d1d}'
            '.rahmen{max-width:640px;margin:0 auto;padding:32px 40px}'
            '.marke{color:#0b7fab;font-size:22px;font-weight:bold}'
            '.box{background:#eef5f9;border-radius:10px;padding:14px 22px;margin:20px 0}'
            '.knopf{display:inline-block;background:#0b7fab;color:#fff;padding:12px 24px;border-radius:4px;'
            'text-decoration:none;font-weight:bold}'
            '.hinweis{color:#666;font-size:12px}'
            '@media (prefers-color-scheme: dark){body{background:#10222c;color:#e8eef2}.marke{color:#5cc8f0}'
            '.box{background:#1d3340;color:#e8eef2}.hinweis{color:#9fb3bf}}'
            '</style></head><body><div class="rahmen">'
            '<div class="marke">Stadtwerke Lindenau</div>'
            '<h1>Abschlag ab Januar</h1>'
            '<p>Diese Mail hat eine eigene Dunkel-Fassung (Hintergrund #10222c). Helle Fassung: weiß.</p>'
            '<div class="box">Neuer Abschlag ab Januar: <b>72,00 € monatlich</b></div>'
            '<p><a class="knopf" href="https://www.gnome.org/" ping="%s/revolution-ping">Zum Kundenportal</a></p>'
            '<p class="hinweis">Testmail für Revolution #354 (eigene Dunkel-Fassung, Link mit ping).</p>'
            '</div></body></html>\n' % REMOTE_HOST)


def anzeige_nachricht(key, betreff):
    m = EmailMessage(policy=email.policy.SMTP)
    if key == 'anzeige-html':
        m.set_content('Ihre Jahresabrechnung 2026 ist da (Textfassung).\n')
        m.add_alternative(anzeige_html(), subtype='html')
    elif key == 'anzeige-latin1':
        m.set_content('Hallo Arnd,\n\nviele Grüße aus Köln! Umlaute in ISO-8859-1: äöü ÄÖÜ ß, '
                      'dazu »Zitat« und ½ Liter.\n\nLink: https://www.gnome.org/ und Adresse '
                      'KONTO_IMAP_1@example.invalid\n\nEinrückung:\n    vier Leerzeichen\n\tein Tab\n\n'
                      '> Zitierte Zeile aus der vorigen Mail\n> mit zweiter Zeile\n\n'
                      'Tschüß\nRevolution #354\n', charset='iso-8859-1')
    elif key == 'anzeige-cid':
        m.set_content('Inline-Bild nur in der HTML-Fassung.\n')
        m.add_alternative('<html><body><p>Inline-Bild aus der Mail (cid:), blaues Quadrat 16×16 '
                          'auf 64×64 skaliert:</p><p><img src="cid:revolution-bild@example.invalid" '
                          'width="64" height="64" alt="Inline-Bild"></p><p>Ende.</p></body></html>\n',
                          subtype='html')
        m.get_payload()[1].add_related(png_bytes(), maintype='image', subtype='png',
                                       cid='<revolution-bild@example.invalid>', filename='bild.png')
    elif key == 'anzeige-dunkel':
        m.set_content('Abschlag ab Januar (Textfassung).\n')
        m.add_alternative(anzeige_dunkel_html(), subtype='html')
    elif key == 'anzeige-remote':
        h = REMOTE_HOST
        m.set_content('Remote-Inhalte nur in der HTML-Fassung.\n')
        m.add_alternative(
            '<html><head><link rel="stylesheet" href="%s/revolution-test.css">'
            '<link rel="preconnect" href="%s"><link rel="dns-prefetch" href="%s">'
            '<style>@import url("%s/revolution-import.css"); .bg{background:url("%s/revolution-bg.png")}</style>'
            '</head><body><p>Remote-Inhalte (dürfen ohne Freigabe nicht laden, #354/#355):</p>'
            '<p class="bg"><img src="%s" alt="Remote-Bild" width="172" height="178"></p>'
            '<img src="%s/revolution-pixel.gif?id=354" width="1" height="1" alt="">'
            '<video src="%s/revolution.webm" width="10" height="10"></video>'
            '<iframe src="%s/revolution-frame.html" width="10" height="10"></iframe>'
            '<p>Ende.</p></body></html>\n' % (h, h, h, h, h, REMOTE_BILD, h, h, h), subtype='html')
    elif key in ('anzeige-remote2', 'anzeige-remote-fremd'):
        m.set_content('Remote-Bild nur in der HTML-Fassung (#355).\n')
        m.add_alternative('<html><body><p>Remote-Bild (%s, #355):</p><p><img src="%s?%s" alt="Remote-Bild" '
                          'width="172" height="178"></p><p>Ende.</p></body></html>\n'
                          % (key, REMOTE_BILD, key), subtype='html')
    absender = ANZEIGE_ANDERE.get(key, ANZEIGE_ABSENDER)
    m['Subject'] = betreff
    m['From'] = email.utils.formataddr(('Revolution Testdaten', absender))
    m['To'] = ANZEIGE_ABSENDER
    m['Cc'] = 'KONTO_IMAP_2@example.invalid'
    m['Date'] = email.utils.formatdate(localtime=True)
    m['Message-ID'] = msgid(key)
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def anzeige(konten, dry):
    """#354: fehlende Anzeige-Mails per APPEND in den Posteingang (idempotent, als gelesen)."""
    for a, k in konten.items():
        nur_testkonten([a])
        k.inbox.refresh_info_sync(cancellable())
        vorhanden = testmails(k.inbox)
        neu = 0
        for key, betreff in ANZEIGE:
            if (betreff, ANZEIGE_ANDERE.get(key, ANZEIGE_ABSENDER)) in vorhanden:
                continue
            print('  %s: %s per APPEND' % (a, betreff))
            neu += 1
            if dry:
                continue
            info = Camel.MessageInfo.new(None)
            info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN)
            k.inbox.append_message_sync(anzeige_nachricht(key, betreff), info, cancellable())
        if neu and not dry:
            k.inbox.synchronize_sync(False, cancellable())
            k.inbox.refresh_info_sync(cancellable())


# #356: Mail mit Anhängen für die Anhangleiste, per APPEND (kein Versand) in den Posteingang der
# Testkonten außer privat@ (Quota fast voll). Inhalte selbst erzeugt und schadlos, fest (gleiche Bytes
# bei jedem Lauf). Erkennung wie ANZEIGE über (Betreff, Absender).
ANHANG_KONTEN = ('KONTO_IMAP_2@example.invalid', 'KONTO_M365@example.invalid', 'KONTO_GOOGLE@example.invalid')
ANHANG_BETREFF = PREFIX + ' Anhänge PDF, Bild, ZIP'
ANHANG_ID = 'anhang-356'


def bild_png():
    """PNG 96×64 mit Farbverlauf (RGB), ein paar KB."""
    w, h = 96, 64
    raw = b''.join(b'\x00' + b''.join(bytes((40 + x * 2, 90 + y * 2, 200 - x)) for x in range(w)) for y in range(h))

    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data
                + struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def zip_bytes():
    """ZIP mit zwei Textdateien, feste Zeitstempel (gleiche Bytes bei jedem Lauf)."""
    puffer = io.BytesIO()
    with zipfile.ZipFile(puffer, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, text in (('liesmich.txt', 'Revolution #356: Testarchiv, schadlos.\n'),
                           ('daten.csv', 'Nr;Name\n1;Lageplan\n2;Marktstand\n')):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 23, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode())
    return puffer.getvalue()


def anhang_nachricht():
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content('Mail mit Anhängen für Revolution #356: PDF, PNG, ZIP, eine Textdatei mit „../“ im '
                  'Dateinamen (Bereinigung) und eine angehängte Nachricht.\n')
    m.add_attachment(pdf_bytes(), maintype='application', subtype='pdf', filename='Lageplan Übersicht.pdf')
    m.add_attachment(bild_png(), maintype='image', subtype='png', filename='Marktstand.png')
    m.add_attachment(zip_bytes(), maintype='application', subtype='zip', filename='Unterlagen.zip')
    m.add_attachment('Dateiname mit Pfadanteil, darf nur als revolution-pfad.txt landen.\n',
                     filename='../../revolution-pfad.txt')
    innen = EmailMessage(policy=email.policy.SMTP)
    innen.set_content('Angehängte Nachricht (message/rfc822) für #356.\n')
    innen['Subject'] = 'Weitergeleitet: Termin Hoffest'
    innen['From'] = email.utils.formataddr(('Revolution Testdaten', ANZEIGE_ABSENDER))
    innen['To'] = ANZEIGE_ABSENDER
    innen['Date'] = 'Fri, 23 Oct 2026 12:00:00 +0200'
    innen['Message-ID'] = msgid('anhang-356-innen')
    m.add_attachment(innen)
    m['Subject'] = ANHANG_BETREFF
    m['From'] = email.utils.formataddr(('Revolution Testdaten', ANZEIGE_ABSENDER))
    m['To'] = ANZEIGE_ABSENDER
    m['Date'] = email.utils.formatdate(localtime=True)
    m['Message-ID'] = msgid(ANHANG_ID)
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def anhang(konten, dry):
    """#356: Anhang-Mail per APPEND in den Posteingang (idempotent, als gelesen), nicht privat@."""
    for a in ANHANG_KONTEN:
        k = konten[a]
        nur_testkonten([a])
        k.inbox.refresh_info_sync(cancellable())
        if (ANHANG_BETREFF, ANZEIGE_ABSENDER) in testmails(k.inbox):
            print('  %s: %s vorhanden' % (a, ANHANG_BETREFF))
            continue
        print('  %s: %s per APPEND' % (a, ANHANG_BETREFF))
        if dry:
            continue
        info = Camel.MessageInfo.new(None)
        info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN)
        k.inbox.append_message_sync(anhang_nachricht(), info, cancellable())
        k.inbox.synchronize_sync(False, cancellable())
        k.inbox.refresh_info_sync(cancellable())


def anhang_teile(k, ziel):
    """#356: nur lesend, ohne Camel: Anhänge der Anhang-Mail vom Server nach ziel schreiben."""
    os.makedirs(ziel, exist_ok=True)
    teile = []
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        c.select('INBOX', readonly=True)
        uids = c.uid('SEARCH', None, 'HEADER', 'Message-ID', msgid(ANHANG_ID))[1][0].split()
        if not uids:
            sys.exit('ABBRUCH: Anhang-Mail fehlt in %s' % k.adresse)
        roh = c.uid('FETCH', uids[-1], '(BODY.PEEK[])')[1][0][1]
        c.logout()
        nachricht = email.message_from_bytes(roh, policy=email.policy.default)
        for teil in nachricht.iter_attachments():
            if teil.get_content_type() == 'message/rfc822':
                teile.append((teil.get_payload()[0]['Subject'] + '.eml', teil.get_payload()[0].as_bytes()))
            else:
                teile.append((teil.get_filename(), teil.get_payload(decode=True)))
    else:
        mails = graph(k, 'GET', "/me/messages?$filter=internetMessageId%20eq%20'"
                      + urllib.request.quote(msgid(ANHANG_ID)) + "'&$select=id")['value']
        if not mails:
            sys.exit('ABBRUCH: Anhang-Mail fehlt in %s' % k.adresse)
        for a in graph(k, 'GET', '/me/messages/%s/attachments' % mails[0]['id'])['value']:
            if 'contentBytes' in a:
                teile.append((a['name'], base64.b64decode(a['contentBytes'])))
            else:
                print('OHNE-BYTES %s %s' % (a['@odata.type'], a['name']))
    for name, daten in teile:
        sicher = name.replace('/', '_')
        with open(os.path.join(ziel, sicher), 'wb') as f:
            f.write(daten)
        print('TEIL %s %d %s' % (hashlib.sha256(daten).hexdigest(), len(daten), name))
        if name.endswith('.txt'):
            # Camel dekodiert text/* mit CRLF → LF (camel-data-wrapper.c:326-335), Evolution ebenso.
            lf = daten.replace(b'\r\n', b'\n')
            print('TEIL-TEXT %s %d %s (CRLF→LF)' % (hashlib.sha256(lf).hexdigest(), len(lf), name))
    return 0


def camel_nachricht(key, absender, betreff, an):
    m = baue(key)
    m['Subject'] = betreff
    m['From'] = absender
    m['To'] = ', '.join(an)
    m['Date'] = email.utils.formatdate(localtime=True)
    m['Message-ID'] = msgid(key)
    if key in ('thread2', 'thread3'):
        vorher = ['thread1'] if key == 'thread2' else ['thread1', 'thread2']
        m['In-Reply-To'] = msgid(vorher[-1])
        m['References'] = ' '.join(msgid(k) for k in vorher)
    data = m.as_bytes()
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(data), None)
    return msg


# #352: Lastmails für die Nachrichtenliste. Absender erfunden (example.org, RFC 2606), nur als
# Kopfzeile; es wird nichts versendet. Datum alle 173 Minuten rückwärts (rund ein Jahr), damit
# alle Formen des relativen Datums vorkommen; jede 5. ungelesen, jede 11. markiert, jede 9. mit
# PNG-Anhang.
LAST_ABSENDER = ['Jana Albrecht', 'Tobias Krämer', 'Stadtwerke Lindenau', 'Miriam Okafor',
                 'Reisebüro Nordwind', 'Felix Brandt', 'Sophie Lange', 'Bücherstube am Markt',
                 'Hausverwaltung Kerner', 'Lukas Weber', 'Emma Schulze', 'Jonas Becker',
                 'Leonie Hoffmann', 'Paul Richter', 'Clara Neumann', 'Ben Wolf', 'Mia Schröder',
                 'Elias Zimmermann', 'Lina Braun', 'Noah Hartmann']
LAST_THEMEN = [
    ('Unterlagen für das Projekt Nordlicht', 'Hallo, anbei die überarbeiteten Pläne und die '
     'Kostenschätzung. Kannst du bis Freitag einen Blick darauf werfen?'),
    ('Kurze Frage zum Termin', 'Passt dir 14 Uhr statt 10 Uhr? Ich hänge morgens noch in einer '
     'Besprechung fest und würde sonst nur halb vorbereitet kommen.'),
    ('Ihre Rechnung', 'Sehr geehrte Kundin, sehr geehrter Kunde, Ihre Rechnung steht ab sofort im '
     'Kundenportal bereit. Den Abschlag haben wir angepasst.'),
    ('Fotos vom Sommerfest', 'Die Bilder sind wirklich schön geworden! Ich habe dir die Auswahl '
     'für die Vereinszeitung in den gemeinsamen Ordner gelegt.'),
    ('Protokoll der Sitzung', 'Liebe Mitglieder, hier das Protokoll vom Montag. Bitte gebt '
     'Korrekturen bis Ende der Woche durch.'),
    ('Abendessen am Samstag?', 'Wir haben spontan einen Tisch beim Italiener an der Ecke '
     'reserviert. Habt ihr Lust mitzukommen?'),
]


def last_nachricht(nr, jetzt):
    name = LAST_ABSENDER[nr % len(LAST_ABSENDER)]
    thema, text = LAST_THEMEN[nr % len(LAST_THEMEN)]
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content('%s\n\nLastmail %04d für die Nachrichtenliste (#352).\n' % (text, nr))
    if nr % 9 == 0:
        m.add_attachment(png_bytes(), maintype='image', subtype='png', filename='last-%04d.png' % nr)
    lokal = re.sub(r'[^a-z]+', '.', name.lower().translate(str.maketrans('äöüß', 'aous'))).strip('.')
    m['Subject'] = '%s Last %04d: %s' % (PREFIX, nr, thema)
    m['From'] = email.utils.formataddr((name, '%s@example.org' % lokal))
    m['To'] = LAST_KONTO
    m['Date'] = email.utils.format_datetime(jetzt - datetime.timedelta(minutes=173 * nr))
    m['Message-ID'] = '<revolution-last-%04d@example.invalid>' % nr
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def last(k, anzahl, dry, ordner=LAST_ORDNER):
    """#352: fehlende Lastmails 1..anzahl per APPEND in ordner (idempotent)."""
    nur_testkonten([k.adresse])
    folder = ordner_holen(k.store, ordner, not dry)
    if folder is None:
        print('  %s: Ordner %s anlegen, %d Lastmails einspielen' % (k.adresse, ordner, anzahl))
        return 0
    folder.refresh_info_sync(cancellable())
    vorhanden = set()
    for betreff, _ in testmails(folder):
        treffer = re.match(re.escape(PREFIX) + r' Last (\d{4}):', betreff)
        if treffer:
            vorhanden.add(int(treffer.group(1)))
    fehlt = [nr for nr in range(1, anzahl + 1) if nr not in vorhanden]
    print('  %s/%s: %d vorhanden, %d fehlen' % (k.adresse, folder.get_full_name(), len(vorhanden), len(fehlt)))
    jetzt = datetime.datetime.now().astimezone()
    for i, nr in enumerate(fehlt):
        if dry:
            break
        info = Camel.MessageInfo.new(None)
        flags = (0 if nr % 5 == 0 else Camel.MessageFlags.SEEN) | (Camel.MessageFlags.FLAGGED if nr % 11 == 0 else 0)
        info.set_flags(Camel.MessageFlags.SEEN | Camel.MessageFlags.FLAGGED, flags)
        # Je APPEND eigenes Zeitlimit; Timer danach beenden (sonst liefen Tausende Threads).
        c = Gio.Cancellable()
        t = threading.Timer(TIMEOUT, c.cancel)
        t.daemon = True
        t.start()
        try:
            folder.append_message_sync(last_nachricht(nr, jetzt), info, c)
        finally:
            t.cancel()
        if (i + 1) % 250 == 0:
            print('  %d von %d eingespielt' % (i + 1, len(fehlt)), flush=True)
    if fehlt and not dry:
        folder.synchronize_sync(False, cancellable())
        folder.refresh_info_sync(cancellable())
    print('LAST %s/%s %d' % (k.account.get_uid(), folder.get_full_name(), folder.get_message_count()))
    return 0


# ---------------------------------------------------------------- Camel/GOA

class TestSession(Camel.Session):
    """CamelSession: Passwort/Token aus GOA, kein Prompt."""

    def setup(self, goa_by_uid):
        self.goa_by_uid = goa_by_uid

    def passwort(self, service):
        obj = self.goa_by_uid[service.get_uid()]
        key = 'smtp-password' if isinstance(service, Camel.Transport) else 'imap-password'
        return obj.get_password_based().call_get_password_sync(key, None)

    def do_get_password(self, service, prompt, item, flags):
        return self.passwort(service)

    def do_get_oauth2_access_token_sync(self, service, c):
        obj = self.goa_by_uid[service.get_uid()]
        token, expires = obj.get_oauth2_based().call_get_access_token_sync(c)
        return True, token, expires

    def do_authenticate_sync(self, service, mechanism, c):
        # Microsoft365-SASL ist nur im Evolution-UI-Modul registriert (PM-Probe, außerhalb Repo);
        # dafür und für XOAUTH2 kein Passwort, sonst Passwort aus GOA setzen.
        authtype = Camel.Sasl.authtype(mechanism) if mechanism else None
        if (not mechanism or (authtype and authtype.need_password)) and not service.get_password():
            service.set_password(self.passwort(service))
        res = service.authenticate_sync(mechanism, c)
        if res == Camel.AuthenticationResult.ACCEPTED:
            return True
        raise GLib.Error('%s: Anmeldung %s abgelehnt (%s)'
                         % (service.get_display_name(), mechanism, res.value_nick))

    def do_get_filter_driver(self, type_, for_folder):
        # Leerer Filter: Evolutions Regeln gelten hier nicht.
        if not hasattr(self, 'filter'):
            self.filter = Camel.FilterDriver.new(self)
        return self.filter

    def do_trust_prompt(self, service, cert, errors):
        return Camel.CertTrust.REJECT_TEMPORARILY


class Konto:
    def __init__(self, adresse, goa_obj, account, transport):
        self.adresse = adresse
        self.goa = goa_obj
        self.account = account
        self.transport = transport
        self.store = None
        self.inbox = None


def konten_finden(registry):
    goa = {}
    for o in Goa.Client.new_sync(None).get_accounts():
        mail = o.get_mail()
        if mail and not o.get_account().props.mail_disabled:
            goa[o.get_account().props.id] = (mail.props.email_address.lower(), o)
    konten = {}
    for coll in registry.list_sources(EDataServer.SOURCE_EXTENSION_GOA):
        acc_id = coll.get_extension(EDataServer.SOURCE_EXTENSION_GOA).get_account_id()
        if acc_id not in goa or goa[acc_id][0] not in TESTKONTEN:
            continue
        adresse, obj = goa[acc_id]
        kids = [s for s in registry.list_sources(None) if s.get_parent() == coll.get_uid()]
        acc = [s for s in kids if s.has_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT)]
        tra = [s for s in kids if s.has_extension(EDataServer.SOURCE_EXTENSION_MAIL_TRANSPORT)]
        if acc and acc[0].get_enabled():
            konten[adresse] = Konto(adresse, obj, acc[0], tra[0] if tra else None)
    fehlt = [a for a in TESTKONTEN if a not in konten]
    if fehlt:
        sys.exit('ABBRUCH: Testkonto nicht in GOA/EDS: %s' % ', '.join(fehlt))
    return konten


def dienst(session, src, ext, typ):
    backend = src.get_extension(ext).get_backend_name()
    service = session.add_service(src.get_uid(), backend, typ)
    src.camel_configure_service(service)
    if isinstance(service, Camel.NetworkService):
        # host-reachable wird sonst erst über die Hauptschleife gesetzt (camel-network-service.c).
        service.can_reach_sync(cancellable())
    service.connect_sync(cancellable())
    if isinstance(service, Camel.OfflineStore):
        service.set_online_sync(True, cancellable())
    return service


class _CFI(ctypes.Structure):
    # GI liefert next/child von CamelFolderInfo nur als Zeiger-int; Layout laut camel-store.h
    # (Muster aus PM-Probeskript, außerhalb Repo).
    _fields_ = [('next', ctypes.c_void_p), ('parent', ctypes.c_void_p), ('child', ctypes.c_void_p),
                ('full_name', ctypes.c_char_p), ('display_name', ctypes.c_char_p),
                ('flags', ctypes.c_uint32), ('unread', ctypes.c_int32), ('total', ctypes.c_int32)]


def ordnerbaum(store):
    """[(full_name, flags)] wie Evolutions Ordnerbaum (RECURSIVE|SUBSCRIBED)."""
    fi = store.get_folder_info_sync(None, Camel.StoreGetFolderInfoFlags.RECURSIVE
                                    | Camel.StoreGetFolderInfoFlags.SUBSCRIBED, cancellable())
    knoten = [(fi.full_name, int(fi.flags))]

    def walk(addr):
        while addr:
            n = _CFI.from_address(addr)
            knoten.append((n.full_name.decode(), n.flags))
            walk(n.child)
            addr = n.next
    walk(fi.child)
    walk(fi.next)
    return knoten


def inbox_holen(store):
    try:
        inbox = store.get_inbox_folder_sync(cancellable())
        # imapx liefert hier eine Instanz außerhalb des Ordner-Caches des Stores; Abruf und
        # Flag-Abgleich laufen aber über die Cache-Instanz. Daher über get_folder_sync holen.
        return store.get_folder_sync(inbox.get_full_name(), 0, cancellable())
    except GLib.Error as e:
        if not e.matches(Camel.store_error_quark(), Camel.StoreError.NO_FOLDER):
            raise
    # microsoft365 hat kein get_inbox_folder_sync; Ordner mit Typ INBOX suchen.
    for name, flags in ordnerbaum(store):
        if flags & Camel.FOLDER_TYPE_MASK == int(Camel.FolderInfoFlags.TYPE_INBOX):
            return store.get_folder_sync(name, 0, cancellable())
    sys.exit('ABBRUCH: kein Posteingang in %s' % store.get_display_name())


def store_oeffnen(session, k):
    print('%s: verbinden' % k.adresse)
    k.store = dienst(session, k.account, EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT,
                     Camel.ProviderType.STORE)
    # imapx kennt Unterordner erst nach LIST (frischer Cache: „No such folder“);
    # wie Evolutions Ordner-Cache zuerst die Ordnerliste abrufen.
    ordnerbaum(k.store)
    k.inbox = inbox_holen(k.store)
    k.inbox.refresh_info_sync(cancellable())


def testmails(folder):
    """{(Betreff, Absender): uid} aller [Revolution-Test]-Mails im Ordner."""
    out = {}
    # Summary durchgehen statt search_by_expression: ohne (match-all …) liefert die Suche 0
    # (camel-folder-search.c, „TODO: else, find all matches“), mit match-all gibt PyGObject
    # die Ergebnis-Strings falsch frei („camel_pstring_free: String is not ours“).
    for uid in folder.get_uids():
        info = folder.get_message_info(uid)
        if info and (info.get_subject() or '').startswith(PREFIX):
            ia = Camel.InternetAddress.new()
            ia.decode(info.get_from())
            out[(info.get_subject(), ia.get(0)[2].lower())] = uid
    return out


def ordner_holen(store, name, anlegen):
    # df.eu legt alle Ordner unter INBOX an (Namespace „INBOX.“), daher auch dort suchen.
    for kandidat in (name, 'INBOX/' + name):
        try:
            return store.get_folder_sync(kandidat, 0, cancellable())
        except GLib.Error as e:
            if not e.matches(Camel.store_error_quark(), Camel.StoreError.NO_FOLDER):
                raise
    if not anlegen:
        return None
    parent, _, kurz = name.rpartition('/')
    eltern = ordner_holen(store, parent, False).get_full_name() if parent else None
    store.create_folder_sync(eltern, kurz, cancellable())
    folder = ordner_holen(store, name, False)
    voll = folder.get_full_name()
    if isinstance(store, Camel.Subscribable) and not store.folder_is_subscribed(voll):
        store.subscribe_folder_sync(voll, cancellable())
    return folder


# ---------------------------------------------------------------- Ablauf

def spam_retten(k, dry):
    """Testmails aus dem Spam-Ordner in den Posteingang verschieben (wie „Kein Spam“)."""
    junk = k.store.get_junk_folder_sync(cancellable())
    if junk is None or isinstance(junk, Camel.VeeFolder):
        # imapx: Junk ist ein virtueller Ordner; Gmail legt Spam in [Gmail]/Spam ab.
        junk = ordner_holen(k.store, '[Gmail]/Spam', False) if 'gmail' in k.adresse else None
    if junk is None:
        return set()
    junk.refresh_info_sync(cancellable())
    uids = list(testmails(junk).items())
    for (betreff, absender), uid in uids:
        print('  %s: %s (von %s) aus %s -> Posteingang' % (k.adresse, betreff, absender,
                                                           junk.get_full_name()))
        if not dry:
            junk.transfer_messages_to_sync([uid], k.inbox, True, cancellable())
    if uids and not dry:
        junk.synchronize_sync(True, cancellable())
        k.inbox.refresh_info_sync(cancellable())
    return {key for key, _ in uids}


def senden(session, konten, dry):
    vorhanden = {a: spam_retten(k, dry) | set(testmails(k.inbox)) for a, k in konten.items()}
    transporte = {}
    gesendet = 0
    for key, absender, betreff, _ in MAILS:
        an = [a for a in TESTKONTEN if (betreff, absender) not in vorhanden[a]]
        if not an:
            continue
        nur_testkonten(an + [absender])
        print('  senden %-10s %s -> %s' % (key, absender, ', '.join(an)))
        gesendet += 1
        if dry:
            continue
        if absender not in transporte:
            transporte[absender] = dienst(session, konten[absender].transport,
                                          EDataServer.SOURCE_EXTENSION_MAIL_TRANSPORT,
                                          Camel.ProviderType.TRANSPORT)
        von = Camel.InternetAddress.new()
        von.add('', absender)
        empf = Camel.InternetAddress.new()
        for a in an:
            empf.add('', a)
        msg = camel_nachricht(key, absender, betreff, an)
        transporte[absender].send_to_sync(msg, von, empf, cancellable())
        time.sleep(2)  # Date-Reihenfolge im Thread
    for t in transporte.values():
        t.disconnect_sync(True, None)
    return gesendet


def warten(konten):
    soll = {(b, a) for _, a, b, _ in MAILS}
    ende = time.monotonic() + WARTEN
    while True:
        offen = {}
        for a, k in konten.items():
            k.inbox.refresh_info_sync(cancellable())
            spam_retten(k, False)
            fehlt = soll - set(testmails(k.inbox))
            if fehlt:
                offen[a] = fehlt
        if not offen:
            return
        if time.monotonic() > ende:
            for a, f in offen.items():
                print('  FEHLT in %s/INBOX: %s' % (a, sorted(b for b, _ in f)))
            sys.exit('ABBRUCH: Zustellung nach %d s unvollständig' % WARTEN)
        time.sleep(15)


def ablegen(konten, dry):
    for a, k in konten.items():
        nur_testkonten([a])
        mails = testmails(k.inbox)
        for key, absender, betreff, gelesen in MAILS:
            uid = mails.get((betreff, absender))
            if uid is None:
                continue
            info = k.inbox.get_message_info(uid)
            ist = bool(info.get_flags() & Camel.MessageFlags.SEEN)
            if ist != gelesen:
                print('  %s: %s -> %s' % (a, betreff, 'gelesen' if gelesen else 'ungelesen'))
                if not dry:
                    info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN if gelesen else 0)
        if not dry:
            k.inbox.synchronize_sync(False, cancellable())
        ordner = {}
        for name in (ORDNER, UNTERORDNER):
            ordner[name] = ordner_holen(k.store, name, not dry)
            if ordner[name] is None:
                print('  %s: Ordner %s anlegen' % (a, name))
        for key, ziel in KOPIEN.items():
            _, absender, betreff, _ = next(m for m in MAILS if m[0] == key)
            uid = mails.get((betreff, absender))
            dest = ordner[ziel]
            if dest is not None:
                dest.refresh_info_sync(cancellable())
                if (betreff, absender) in testmails(dest):
                    continue
            print('  %s: %s -> %s kopieren' % (a, betreff, ziel))
            if not dry and uid is not None:
                k.inbox.transfer_messages_to_sync([uid], dest, False, cancellable())
                dest.synchronize_sync(False, cancellable())


def teile(part, tiefe, out):
    dw = part.get_content()
    ct = part.get_content_type()
    typ = '%s/%s' % (ct.type, ct.subtype)
    if isinstance(dw, Camel.Multipart):
        out.append((tiefe, typ, None, None))
        for i in range(dw.get_number()):
            teile(dw.get_part(i), tiefe + 1, out)
        return
    extra = ct.param('method') if typ == 'text/calendar' else None
    if typ == 'text/html':
        stream = Camel.StreamMem.new()
        dw.decode_to_stream_sync(stream, None)
        html = bytes(stream.get_byte_array()).decode('utf-8', 'replace')
        extra = 'Remote-Bild=%s' % ('ja' if 'src="%s"' % REMOTE_BILD in html else 'nein')
    out.append((tiefe, typ, part.get_filename(), extra))


def pruefen(konten):
    fehler = 0
    for a, k in konten.items():
        print('=' * 70)
        print('%s  (Quelle %s, Backend %s)' % (a, k.account.get_uid()[:8],
              k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name()))
        k.inbox.refresh_info_sync(cancellable())
        mails = testmails(k.inbox)
        print('  %s: %d [Revolution-Test]-Mails' % (k.inbox.get_full_name(), len(mails)))
        for key, absender, betreff, gelesen in MAILS:
            uid = mails.get((betreff, absender))
            if uid is None:
                print('  FEHLT  %s (%s)' % (betreff, absender))
                fehler += 1
                continue
            seen = bool(k.inbox.get_message_info(uid).get_flags() & Camel.MessageFlags.SEEN)
            ok = seen == gelesen
            fehler += not ok
            print('  %s %-9s %s  von %s' % ('ok  ' if ok else 'FLAG', 'gelesen' if seen else 'ungelesen',
                                           betreff, absender))
            if key in ('html', 'anhang', 'thread3', 'einladung'):
                msg = k.inbox.get_message_sync(uid, cancellable())
                if key == 'thread3':
                    print('         In-Reply-To=%s References=%s' % (
                        msg.get_header('In-Reply-To'), (msg.get_header('References') or '').split()))
                    continue
                out = []
                teile(msg, 0, out)
                for tiefe, typ, name, extra in out:
                    print('         %s%s%s%s' % ('  ' * tiefe, typ, ' name=%s' % name if name else '',
                                                 ' %s' % extra if extra else ''))
        for key, betreff in ANZEIGE:
            uid = mails.get((betreff, ANZEIGE_ABSENDER))
            if uid is None:
                print('  FEHLT  %s (%s)' % (betreff, ANZEIGE_ABSENDER))
                fehler += 1
                continue
            print('  ok   %s  UID %s' % (betreff, uid))
            out = []
            teile(k.inbox.get_message_sync(uid, cancellable()), 0, out)
            for tiefe, typ, name, extra in out:
                print('         %s%s%s' % ('  ' * tiefe, typ, ' name=%s' % name if name else ''))
        for name in (ORDNER, UNTERORDNER):
            f = ordner_holen(k.store, name, False)
            if f is None:
                print('  FEHLT  Ordner %s' % name)
                fehler += 1
                continue
            f.refresh_info_sync(cancellable())
            sub = f.get_full_name() in [n for n, _ in ordnerbaum(k.store)]
            print('  Ordner %-34s im Ordnerbaum=%s  Testmails: %s' % (
                f.get_full_name(), sub, sorted(b for b, _ in testmails(f))))
            fehler += not sub
    print('=' * 70)
    print('Prüfung: %d Fehler' % fehler)
    return fehler


def ordner_konten(registry):
    """#348: alle aktivierten Mail-Konten aus GOA wie die Seitenleiste (Online-Konto mit Mail,
    Quelle und Collection aktiviert, mail-account.c), ohne Einschränkung auf Testkonten."""
    goa = {}
    for o in Goa.Client.new_sync(None).get_accounts():
        mail = o.get_mail()
        if mail and not o.get_account().props.mail_disabled:
            goa[o.get_account().props.id] = (mail.props.email_address.lower(), o)
    konten = []
    for coll in registry.list_sources(EDataServer.SOURCE_EXTENSION_GOA):
        acc_id = coll.get_extension(EDataServer.SOURCE_EXTENSION_GOA).get_account_id()
        if acc_id not in goa or not coll.get_enabled():
            continue
        for s in registry.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT):
            if s.get_parent() == coll.get_uid() and s.get_enabled():
                konten.append(Konto(goa[acc_id][0], goa[acc_id][1], s, None))
    return konten


def ordner(session, konten):
    """#348: nur lesend. Verbinden und camel_store_get_folder_info_sync mit denselben Flags
    wie die App (RECURSIVE|SUBSCRIBED, main.c note_store_thread); keine Schreibzugriffe."""
    for k in konten:
        store = dienst(session, k.account, EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT,
                       Camel.ProviderType.STORE)
        namen = [n for n, _ in ordnerbaum(store)]
        for n in namen:
            print('ORDNER %s %s' % (k.account.get_uid(), n))
        print('ANZAHL %s %d %s' % (k.account.get_uid(), len(namen), k.adresse))
        store.disconnect_sync(True, None)
    return 0


# ---------------------------------------------------------------- #353: Flags direkt am Server

GRAPH = 'https://graph.microsoft.com/v1.0'
EINLADUNG = PREFIX + ' Einladung: Abstimmung Testdaten'
LIVE = PREFIX + ' Live'


def imap_verbinden(k):
    """Eigene IMAP-Verbindung mit den GOA-Daten (Host, Benutzer, Passwort bzw. OAuth2)."""
    m = k.goa.get_mail()
    host, _, port = m.props.imap_host.partition(':')
    if m.props.imap_use_ssl:
        c = imaplib.IMAP4_SSL(host, int(port or 993), timeout=TIMEOUT)
    else:
        c = imaplib.IMAP4(host, int(port or 143), timeout=TIMEOUT)
        if m.props.imap_use_tls:
            c.starttls()
    user = m.props.imap_user_name
    if k.goa.get_oauth2_based():
        token = k.goa.get_oauth2_based().call_get_access_token_sync(None)[0]
        c.authenticate('XOAUTH2', lambda _: ('user=%s\x01auth=Bearer %s\x01\x01' % (user, token)).encode())
    else:
        c.login(user, k.goa.get_password_based().call_get_password_sync('imap-password', None))
    return c


def imap_ordner(c, ziel=ORDNER):
    """(IMAP-Name, Camel-Name) des Ordners Revolution-Test (df.eu: unter INBOX mit Trenner „.“)."""
    if ziel == 'INBOX':
        return 'INBOX', 'INBOX'
    typ, daten = c.list()
    for zeile in daten:
        treffer = re.match(rb'\((.*?)\) "(.*?)" "?(.*?)"?$', zeile)
        if treffer and treffer.group(3).decode().split(treffer.group(2).decode())[-1] == ziel:
            name = treffer.group(3).decode()
            return name, name.replace(treffer.group(2).decode(), '/')
    sys.exit('ABBRUCH: Ordner %s fehlt' % ziel)


def imap_mails(c):
    """[(UID, gelesen, markiert, Betreff)] der Testmails im gewählten Ordner."""
    typ, daten = c.uid('SEARCH', None, 'ALL')
    out = []
    for uid in daten[0].split():
        typ, d = c.uid('FETCH', uid, '(FLAGS BODY.PEEK[HEADER.FIELDS (SUBJECT)])')
        flags = d[0][0].decode()
        betreff = str(email.header.make_header(email.header.decode_header(
            d[0][1].decode().partition(':')[2].strip())))
        if betreff.startswith(PREFIX):
            out.append((uid.decode(), '\\Seen' in flags, '\\Flagged' in flags, betreff))
    return out


def graph(k, methode, pfad, daten=None):
    token = k.goa.get_oauth2_based().call_get_access_token_sync(None)[0]
    req = urllib.request.Request(GRAPH + pfad, method=methode,
                                 data=json.dumps(daten).encode() if daten is not None else None,
                                 headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read() or b'null')


def graph_mails(k):
    """(Ordner-ID, [(ID, gelesen, markiert, Betreff, flagStatus)]) in Revolution-Test."""
    ordner = graph(k, 'GET', "/me/mailFolders?$top=100&$filter=displayName%20eq%20'" + ORDNER + "'")['value']
    if not ordner:
        sys.exit('ABBRUCH: Ordner %s fehlt' % ORDNER)
    mails = graph(k, 'GET', '/me/mailFolders/%s/messages?$top=100&$select=subject,isRead,importance,flag'
                  % ordner[0]['id'])['value']
    return ordner[0]['id'], [(m['id'], m['isRead'], m['importance'] == 'high', m['subject'],
                              m['flag']['flagStatus']) for m in mails if m['subject'].startswith(PREFIX)]


def flags(k):
    """#353: nur lesend (IMAP EXAMINE, Graph GET)."""
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        name, camel = imap_ordner(c)
        c.select('"%s"' % name, readonly=True)
        print('ORDNER %s/%s' % (k.account.get_uid(), camel))
        for uid, seen, flagged, betreff in imap_mails(c):
            print('FLAGS %d %d %s' % (seen, flagged, betreff))
        c.logout()
    else:
        _, mails = graph_mails(k)
        print('ORDNER %s/%s' % (k.account.get_uid(), ORDNER))
        for _, seen, flagged, betreff, status in mails:
            print('FLAGS %d %d %s (flag.flagStatus=%s)' % (seen, flagged, betreff, status))
    return 0


def ungelesen(k, ziel, soll=None):
    """#353: ungelesene UIDs eines IMAP-Ordners; mit soll (Datei mit einer früheren Ausgabe)
    diesen Stand wiederherstellen."""
    c = imap_verbinden(k)
    name, _ = imap_ordner(c, ziel)
    c.select('"%s"' % name, readonly=soll is None)
    ist = set(c.uid('SEARCH', None, 'UNSEEN')[1][0].split())
    if soll is not None:
        nur_testkonten([k.adresse])
        zeile = next(z for z in open(soll) if z.startswith('UNGELESEN '))
        ziel_uids = {u.encode() for u in zeile.split()[2:]}
        for uids, aenderung in ((ist - ziel_uids, '+FLAGS'), (ziel_uids - ist, '-FLAGS')):
            if uids:
                menge = b','.join(sorted(uids, key=int)).decode()
                print('%s \\Seen %s: %s' % (aenderung, menge, c.uid('STORE', menge, aenderung, '(\\Seen)')[0]))
        ist = set(c.uid('SEARCH', None, 'UNSEEN')[1][0].split())
    c.logout()
    print('UNGELESEN %d %s' % (len(ist), ' '.join(sorted((u.decode() for u in ist), key=int))))
    return 0


def server(k, aktion, ziel=ORDNER, warte=None):
    """#353: serverseitige Änderung in einem Testkonto, während die App läuft; neu/weg auch im
    Posteingang (ziel INBOX, Beleg für IDLE)."""
    nur_testkonten([k.adresse])
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        name, _ = imap_ordner(c, ziel)
        c.select('"%s"' % name)
        if warte:
            print('BEREIT', flush=True)
            while not os.path.exists(warte):
                time.sleep(0.005)
        if aktion == 'neu':
            m = EmailMessage(policy=email.policy.SMTP)
            m.set_content('Live-Mail für #353 (APPEND, wird danach gelöscht).\n')
            m['Subject'] = '%s %s' % (LIVE, datetime.datetime.now().strftime('%H:%M:%S'))
            m['From'] = k.adresse
            m['To'] = k.adresse
            m['Date'] = email.utils.formatdate(localtime=True)
            print(c.append('"%s"' % name, None, None, m.as_bytes()))
            if warte:
                print('ABGELEGT ' + datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3], flush=True)
        elif aktion == 'weg':
            for uid, _, _, betreff in imap_mails(c):
                if betreff.startswith(LIVE):
                    print(c.uid('STORE', uid, '+FLAGS', '(\\Deleted)'))
            print(c.expunge())
        else:
            uid = next(u for u, _, _, b in imap_mails(c) if b == EINLADUNG)
            flag = '\\Seen' if aktion in ('gelesen', 'ungelesen') else '\\Flagged'
            print(c.uid('STORE', uid, '+FLAGS' if aktion in ('gelesen', 'markiert') else '-FLAGS', '(%s)' % flag))
        c.logout()
    else:
        if aktion in ('neu', 'weg'):
            sys.exit('ABBRUCH: %s nur für IMAP' % aktion)
        _, mails = graph_mails(k)
        mid = next(m[0] for m in mails if m[3] == EINLADUNG)
        daten = {'gelesen': {'isRead': True}, 'ungelesen': {'isRead': False},
                 'markiert': {'importance': 'high'}, 'unmarkiert': {'importance': 'normal'}}[aktion]
        graph(k, 'PATCH', '/me/messages/' + mid, daten)
        print('PATCH %s' % daten)
    return 0


# ---------------------------------------------------------------- #350: Ungelesen-Zähler

def mutf7_decode(name):
    """IMAP-Ordnername (modifiziertes UTF-7, RFC 3501 5.1.3) → Text."""
    def teil(m):
        if m.group(1) == '':
            return '&'
        b = m.group(1).replace(',', '/')
        return base64.b64decode(b + '=' * (-len(b) % 4)).decode('utf-16-be')
    return re.sub(r'&([^-]*)-', teil, name)


def zaehler_imap(k):
    """{voller Name wie Camel: n} per LIST und STATUS (UNSEEN), nur lesend (kein SELECT/EXAMINE)."""
    c = imap_verbinden(k)
    try:
        out = {}
        typ, zeilen = c.list('""', '*')
        for z in zeilen:
            if not isinstance(z, bytes):
                continue
            m = re.match(rb'\((?P<flags>[^)]*)\) (?P<delim>"[^"]*"|NIL) (?P<name>.*)$', z)
            if not m or b'\\noselect' in m.group('flags').lower() or b'\\nonexistent' in m.group('flags').lower():
                continue
            roh = m.group('name').decode()
            delim = m.group('delim').decode().strip('"')
            typ, st = c.status(roh if roh.startswith('"') else '"%s"' % roh, '(UNSEEN)')
            n = int(re.search(rb'UNSEEN (\d+)', st[0]).group(1))
            name = mutf7_decode(roh.strip('"'))
            out[name.replace(delim, '/') if delim != 'NIL' else name] = n
        return out
    finally:
        c.logout()


def anzahl(konten, uid, ziel):
    """#410: nur lesend, unabhängig von Camel: Nachrichten eines IMAP-Ordners per LIST, EXAMINE
    (EXISTS) und UID SEARCH; „sichtbar“ ohne \\Deleted und Junk wie die Liste der App
    (mail-view.c hidden_flags). Zeile „ANZAHL <UID>/<Ordner> <sichtbar> EXISTS <n>“."""
    k = next((k for k in konten if k.account.get_uid() == uid), None)
    if k is None:
        sys.exit('ABBRUCH: Konto %s nicht in GOA/EDS' % uid)
    if k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name() == 'microsoft365':
        # #414: Graph zählt alle Nachrichten des Ordners (totalItemCount, nur GET).
        ref = zaehler_graph(k, 'totalItemCount')
        if ziel not in ref:
            sys.exit('ABBRUCH: Ordner %s fehlt' % ziel)
        print('ANZAHL %s/%s %d EXISTS %d' % (uid, ziel, ref[ziel], ref[ziel]))
        return 0
    c = imap_verbinden(k)
    try:
        roh = None
        for z in c.list('""', '*')[1]:
            m = re.match(rb'\((?P<flags>[^)]*)\) (?P<delim>"[^"]*"|NIL) (?P<name>.*)$', z) \
                if isinstance(z, bytes) else None
            if not m:
                continue
            delim = m.group('delim').decode().strip('"')
            name = mutf7_decode(m.group('name').decode().strip('"'))
            if (name.replace(delim, '/') if delim != 'NIL' else name) == ziel:
                roh = m.group('name').decode()
        if roh is None:
            sys.exit('ABBRUCH: Ordner %s fehlt' % ziel)
        typ, d = c.select(roh if roh.startswith('"') else '"%s"' % roh, readonly=True)
        exists = int(d[0])
        sichtbar = len(c.uid('SEARCH', None, 'UNDELETED', 'UNKEYWORD', 'Junk', 'UNKEYWORD', '$Junk')[1][0].split())
    finally:
        c.logout()
    print('ANZAHL %s/%s %d EXISTS %d' % (uid, ziel, sichtbar, exists))
    return 0


def zaehler_graph(k, feld='unreadItemCount'):
    """{Pfad der Anzeigenamen wie Camel microsoft365: n} per Graph, nur GET; #414: feld
    totalItemCount für --anzahl."""
    q = '?' + urllib.parse.urlencode({'$select': 'displayName,childFolderCount,' + feld, '$top': '100'})
    out = {}

    def holen(pfad, prefix):
        while pfad:
            d = graph(k, 'GET', pfad)
            for f in d['value']:
                name = prefix + f['displayName']
                out[name] = f[feld]
                if f.get('childFolderCount'):
                    holen('/me/mailFolders/%s/childFolders%s' % (f['id'], q), name + '/')
            pfad = d.get('@odata.nextLink', '').removeprefix(GRAPH) or None

    holen('/me/mailFolders' + q, '')
    return out


def zaehler(konten, log):
    """#350: nur lesend. Ungelesene je Ordner unabhängig von Camel; mit log Vergleich mit dem
    letzten „Ungelesen: <UID> <Ordner> <N>“ der App je Ordner."""
    app = {}
    if log:
        for z in open(log, encoding='utf-8', errors='replace'):
            m = re.search(r'revolution-DEBUG: [0-9:.]+: Ungelesen: (\S+) (.+) (\d+)$', z.rstrip('\n'))
            if m:
                app[(m.group(1), m.group(2))] = int(m.group(3))
    fehler = gleich = ohne = 0
    for k in konten:
        uid = k.account.get_uid()
        backend = k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name()
        ref = zaehler_graph(k) if backend == 'microsoft365' else zaehler_imap(k)
        for name in sorted(ref):
            print('ZAEHLER %s %s %d' % (uid, name, ref[name]))
            if not log:
                continue
            if (uid, name) not in app:
                # App ohne Zahl: imapx-Ordner ohne Abgleich in dieser Sitzung (main.c forget_unread)
                print('  OHNE ZAHL in der App: %s %s (Server %d)' % (k.adresse, name, ref[name]))
                ohne += 1
            elif app[(uid, name)] != ref[name]:
                print('  ABWEICHUNG %s %s: App %d, Server %d' % (k.adresse, name, app[(uid, name)], ref[name]))
                fehler += 1
            else:
                gleich += 1
    if log:
        print('Vergleich: %d gleich, %d abweichend, %d ohne Zahl in der App' % (gleich, fehler, ohne))
    return 1 if fehler else 0


# ---------------------------------------------------------------- #351: Kombinierter Eingang

FAVORITEN = 'revolution-favoriten'
SICHTBAR = ('UNDELETED', 'UNKEYWORD', 'Junk', 'UNKEYWORD', '$Junk')


def posteingang(k):
    """(sichtbar, ungelesen) im Posteingang, nur lesend: IMAP EXAMINE + UID SEARCH ohne Gelöschte und
    Junk wie die Liste der App (mail-favorites.c VISIBLE), microsoft365 Graph GET."""
    backend = k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name()
    if backend == 'microsoft365':
        d = graph(k, 'GET', '/me/mailFolders/inbox?$select=totalItemCount,unreadItemCount')
        return d['totalItemCount'], d['unreadItemCount']
    c = imap_verbinden(k)
    try:
        c.select('INBOX', readonly=True)
        n = len(c.uid('SEARCH', None, *SICHTBAR)[1][0].split())
        u = len(c.uid('SEARCH', None, 'UNSEEN', *SICHTBAR)[1][0].split())
    finally:
        c.logout()
    return n, u


def kombiniert(konten, log):
    """#351: nur lesend. Je Konto Posteingang vom Server; mit log Vergleich mit der App."""
    app, liste, fav_u = {}, None, None
    if log:
        for z in open(log, encoding='utf-8', errors='replace'):
            m = re.search(r'revolution-DEBUG: [0-9:.]+: (.*)$', z.rstrip('\n'))
            if not m:
                continue
            t = m.group(1)
            a = re.match(r'Favoriten: Anzahl inbox (\S+) (.+) (\d+)$', t)
            if a:
                app[a.group(1)] = int(a.group(3))
            a = re.match(r'Nachrichten: %s inbox (?:Cache |Server |vollständig |.*Liste )(\d+)' % FAVORITEN, t)
            if a:
                liste = int(a.group(1))
            a = re.match(r'Ungelesen: %s inbox (\d+)$' % FAVORITEN, t)
            if a:
                fav_u = int(a.group(1))
    fehler = 0
    summe_n = summe_u = 0
    for k in konten:
        uid = k.account.get_uid()
        n, u = posteingang(k)
        summe_n += n
        summe_u += u
        print('KOMBINIERT %s %s %d %d' % (uid, k.adresse, n, u))
        if log:
            if app.get(uid) != n:
                print('  ABWEICHUNG %s: App %s, Server %d' % (k.adresse, app.get(uid), n))
                fehler += 1
            else:
                print('  GLEICH %s: App %d = Server %d' % (k.adresse, n, n))
    print('SUMME %d Konten, %d sichtbar, %d ungelesen' % (len(konten), summe_n, summe_u))
    if log:
        for was, ist, soll in (('Liste „Alle Eingänge“', liste, summe_n), ('Ungelesen „Alle Eingänge“', fav_u, summe_u)):
            print('  %s %s: App %s, Server %d' % ('GLEICH' if ist == soll else 'ABWEICHUNG', was, ist, soll))
            fehler += ist != soll
        if len(app) != len(konten):
            print('  ABWEICHUNG Konten in „Alle Eingänge“: App %d, GOA %d' % (len(app), len(konten)))
            fehler += 1
        print('Vergleich: %d Abweichungen' % fehler)
    return 1 if fehler else 0


def live_flags(k):
    """#351: nur lesend (EXAMINE INBOX)."""
    c = imap_verbinden(k)
    try:
        c.select('INBOX', readonly=True)
        for uid in c.uid('SEARCH', None, 'SUBJECT', '"%s"' % LIVE)[1][0].split():
            flags = c.uid('FETCH', uid, '(FLAGS)')[1][0].decode()
            print('FLAGS %d %d %s' % ('\\Seen' in flags, '\\Flagged' in flags, uid.decode()))
    finally:
        c.logout()
    return 0


def main():
    ap = argparse.ArgumentParser(description='Testdaten für Revolution (#344)')
    ap.add_argument('--dry-run', action='store_true', help='nur anzeigen, nichts senden/schreiben')
    ap.add_argument('--pruefen', action='store_true', help='nur lesend prüfen')
    ap.add_argument('--ordner', action='store_true', help='nur lesend: Ordnerliste aller Konten')
    ap.add_argument('--anzeige', action='store_true',
                    help='#354: nur die Anzeige-Mails per APPEND in die Posteingänge')
    ap.add_argument('--anhang', action='store_true',
                    help='#356: nur die Anhang-Mail per APPEND (geops@, outlook.com, Gmail)')
    ap.add_argument('--anhang-teile', nargs=2, metavar=('KONTO', 'ORDNER'),
                    help='#356: nur lesend die Anhänge der Anhang-Mail vom Server nach ORDNER')
    ap.add_argument('--last', type=int, metavar='N',
                    help='N Lastmails per APPEND in %s von %s (#352)' % (LAST_ORDNER, LAST_KONTO))
    ap.add_argument('--last-ordner', default=LAST_ORDNER, metavar='NAME',
                    help='#350: Ordner für --last statt %s (z. B. Revolution-Zaehler)' % LAST_ORDNER)
    ap.add_argument('--flags', metavar='KONTO', help='#353: nur lesend Flags in %s vom Server' % ORDNER)
    ap.add_argument('--ungelesen', nargs=2, metavar=('KONTO', 'ORDNER'),
                    help='#353: ungelesene UIDs (nur lesend); mit --soll DATEI wiederherstellen')
    ap.add_argument('--zaehler', nargs='?', const='', metavar='LOG',
                    help='#350: nur lesend Ungelesene je Ordner (IMAP STATUS, Graph), Vergleich mit LOG')
    ap.add_argument('--anzahl', nargs=2, metavar=('UID', 'ORDNER'),
                    help='#410: nur lesend Nachrichten eines Ordners (IMAP EXAMINE, UID SEARCH; #414 Graph)')
    ap.add_argument('--warte', metavar='DATEI', help='#414: --server erst ausführen, wenn DATEI existiert')
    ap.add_argument('--soll', metavar='DATEI', help='#353: Ausgabe von --ungelesen als Sollstand')
    ap.add_argument('--server', nargs='+', metavar='KONTO AKTION [INBOX]',
                    help='#353: serverseitig gelesen|ungelesen|markiert|unmarkiert|neu|weg [im Posteingang]')
    ap.add_argument('--kombiniert', nargs='?', const='', metavar='LOG',
                    help='#351: nur lesend Posteingänge aller Konten (EXAMINE, Graph), Vergleich mit LOG')
    ap.add_argument('--live-flags', metavar='KONTO', help='#351: nur lesend Flags der Live-Mails im Posteingang')
    args = ap.parse_args()

    if args.live_flags:
        adresse = args.live_flags.lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        if not k.goa.get_mail().props.imap_supported:
            ap.error('--live-flags nur für IMAP')
        return live_flags(k)

    if args.ungelesen:
        adresse = args.ungelesen[0].lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return ungelesen(k, args.ungelesen[1], args.soll)
    if args.anhang_teile:
        adresse = args.anhang_teile[0].lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return anhang_teile(k, args.anhang_teile[1])
    if args.flags or args.server:
        adresse = (args.flags or args.server[0]).lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        if args.server and (len(args.server) not in (2, 3) or args.server[2:] not in ([], ['INBOX'])):
            ap.error('--server KONTO AKTION [INBOX]')
        return flags(k) if args.flags else server(k, *args.server[1:], warte=args.warte)

    os.makedirs(os.path.join(CACHE, 'data'), exist_ok=True)
    os.makedirs(os.path.join(CACHE, 'cache'), exist_ok=True)
    Camel.init(CACHE, False)
    Camel.provider_init()
    EDataServer.SourceCamel.register_types()
    registry = EDataServer.SourceRegistry.new_sync(None)
    if args.anzahl:
        return anzahl(ordner_konten(registry), *args.anzahl)
    if args.zaehler is not None:
        return zaehler(ordner_konten(registry), args.zaehler)
    if args.kombiniert is not None:
        return kombiniert(ordner_konten(registry), args.kombiniert)
    if args.ordner:
        konten = ordner_konten(registry)
        session = TestSession(user_data_dir=os.path.join(CACHE, 'data'),
                              user_cache_dir=os.path.join(CACHE, 'cache'))
        session.setup({k.account.get_uid(): k.goa for k in konten})
        session.set_online(True)
        return ordner(session, konten)
    konten = konten_finden(registry)
    nur_testkonten(list(konten))
    for k in konten.values():
        nur_testkonten([k.account.get_extension(EDataServer.SOURCE_EXTENSION_AUTHENTICATION).get_user()])

    session = TestSession(user_data_dir=os.path.join(CACHE, 'data'),
                          user_cache_dir=os.path.join(CACHE, 'cache'))
    session.setup({s.get_uid(): k.goa for k in konten.values()
                   for s in (k.account, k.transport) if s})
    session.set_online(True)
    if args.last:
        k = konten[LAST_KONTO]
        k.store = dienst(session, k.account, EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT,
                         Camel.ProviderType.STORE)
        ordnerbaum(k.store)
        n = last(k, args.last, args.dry_run, args.last_ordner)
        k.store.disconnect_sync(True, None)
        return n
    if args.anhang:
        for a in ANHANG_KONTEN:
            store_oeffnen(session, konten[a])
        anhang(konten, args.dry_run)
        for a in ANHANG_KONTEN:
            konten[a].store.disconnect_sync(True, None)
        return 0
    for k in konten.values():
        store_oeffnen(session, k)

    if args.pruefen:
        return 1 if pruefen(konten) else 0
    if args.anzeige:
        anzeige(konten, args.dry_run)
        for k in konten.values():
            k.store.disconnect_sync(True, None)
        return 0
    n = senden(session, konten, args.dry_run)
    if n and not args.dry_run:
        print('  warte auf Zustellung …')
        warten(konten)
    ablegen(konten, args.dry_run)
    anzeige(konten, args.dry_run)
    anhang(konten, args.dry_run)
    for k in konten.values():
        k.store.disconnect_sync(True, None)
    return 0


if __name__ == '__main__':
    sys.exit(main())
