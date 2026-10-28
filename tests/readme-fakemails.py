#!/usr/bin/env python3
# readme-fakemails.py – erfundene Mails für die README-Screenshots (data/screenshots/).
#
# Legt per IMAP APPEND (kein Versand) zwölf Vorgänge (14 Mails) in den Posteingang des ersten
# Testkontos mit typ=imap und freigabe=schreibend aus ~/.config/revolution/testkonten.ini
# (Vorlage tests/testkonten.ini.example). Absender erfunden (Domains .example/.invalid), Datum
# heute und die letzten Tage, ungelesen bis auf zwei. Erkennung über die Message-ID: Lokalteil
# beginnt mit „readme-fake-“; vorhandene Mails mit dieser ID werden nicht doppelt angelegt,
# andere Mails im Postfach nie angefasst.
#
# Aufruf:
#   tests/readme-fakemails.py             anlegen (fehlende ergänzen)
#   tests/readme-fakemails.py --liste     nur lesend: vorhandene Fake-Mails zählen
#   tests/readme-fakemails.py --loeschen  Fake-Mails (nur readme-fake-*) löschen (\Deleted + UID EXPUNGE)
#
# Zugangsdaten nur zur Laufzeit aus GOA (wie tests/testdaten.py, imap_verbinden), nie ausgegeben.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import configparser
import datetime
import email.policy
import email.utils
import imaplib
import io
import os
import re
import sys
from email.message import EmailMessage

import gi
gi.require_version('Goa', '1.0')
from gi.repository import Goa  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testkonten import PFAD  # noqa: E402  (bricht ab, wenn die INI fehlt)

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

TIMEOUT = 90
KENNUNG = 'readme-fake-'
FONT_DIR = '/usr/share/fonts/truetype/noto/'
ZONE = datetime.timezone(datetime.timedelta(hours=1))  # CET (Ende Oktober)
HEUTE = datetime.datetime.now(ZONE).replace(second=0, microsecond=0)


def konto_adresse():
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(PFAD, encoding='utf-8')
    for name in ini.sections():
        if ini[name].get('typ') == 'imap' and ini[name].get('freigabe') == 'schreibend':
            return ini[name]['adresse'].lower()
    sys.exit('FEHLER  %s: kein Konto mit typ=imap, freigabe=schreibend' % PFAD)


def imap_verbinden(adresse):
    """Wie tests/testdaten.py imap_verbinden: Host, Benutzer, Passwort bzw. OAuth2 aus GOA."""
    for o in Goa.Client.new_sync(None).get_accounts():
        m = o.get_mail()
        if m and m.props.email_address.lower() == adresse:
            break
    else:
        sys.exit('FEHLER  Testkonto nicht in GOA')
    host, _, port = m.props.imap_host.partition(':')
    if m.props.imap_use_ssl:
        c = imaplib.IMAP4_SSL(host, int(port or 993), timeout=TIMEOUT)
    else:
        c = imaplib.IMAP4(host, int(port or 143), timeout=TIMEOUT)
        if m.props.imap_use_tls:
            c.starttls()
    user = m.props.imap_user_name
    if o.get_oauth2_based():
        token = o.get_oauth2_based().call_get_access_token_sync(None)[0]
        c.authenticate('XOAUTH2', lambda _: ('user=%s\x01auth=Bearer %s\x01\x01' % (user, token)).encode())
    else:
        c.login(user, o.get_password_based().call_get_password_sync('imap-password', None))
    return c


# ---------------------------------------------------------------- Bilder und Anhänge

def schrift(name, groesse):
    return ImageFont.truetype(FONT_DIR + name, groesse)


def banner_png():
    """600×180, Farbverlauf in Herbsttönen, Schriftzug „Herbstmarkt 2026“."""
    w, h = 600, 180
    im = Image.new('RGB', (w, h))
    links, rechts = (122, 38, 18), (226, 140, 44)
    px = im.load()
    for x in range(w):
        for y in range(h):
            t = (x / w) * 0.8 + (y / h) * 0.2
            px[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(links, rechts))
    d = ImageDraw.Draw(im, 'RGBA')
    for (cx, cy, r, farbe) in ((520, 30, 46, (255, 214, 120, 70)), (470, 150, 34, (170, 60, 20, 90)),
                               (565, 120, 26, (255, 190, 90, 80)), (40, 160, 30, (255, 170, 60, 50)),
                               (95, 18, 18, (255, 220, 150, 60))):
        d.ellipse((cx - r, cy - r * 0.6, cx + r, cy + r * 0.6), fill=farbe)
    d.text((40, 92), 'Herbstmarkt 2026', font=schrift('NotoSerif-Bold.ttf', 46), fill=(255, 248, 236),
           anchor='ls')
    d.text((42, 128), 'Rathausplatz · 14. und 15. November', font=schrift('NotoSans-Regular.ttf', 19),
           fill=(255, 232, 205), anchor='ls')
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def foto_jpg(farben, titel, unter):
    """800×600, farbige Flächen mit Text."""
    im = Image.new('RGB', (800, 600), farben[0])
    d = ImageDraw.Draw(im)
    d.rectangle((0, 380, 800, 600), fill=farben[1])
    d.rectangle((90, 210, 420, 470), fill=farben[2])
    d.rectangle((460, 250, 720, 470), fill=farben[3])
    d.polygon(((70, 220), (255, 120), (440, 220)), fill=farben[4])
    d.text((40, 70), titel, font=schrift('NotoSans-Bold.ttf', 44), fill=(255, 255, 255), anchor='ls')
    d.text((40, 560), unter, font=schrift('NotoSans-Regular.ttf', 24), fill=(255, 255, 255), anchor='ls')
    buf = io.BytesIO()
    im.save(buf, 'JPEG', quality=88)
    return buf.getvalue()


def rechnung_pdf():
    """Ein-Seiten-PDF „Rechnung“, von Hand (Muster: tests/testdaten.py pdf_bytes)."""
    zeilen = [(18, 760, 'Rechnung R-2026-1187'), (11, 730, 'Lichtwerk GmbH, Am Hafen 3, 53111 Bonn'),
              (11, 700, 'Leistung: Beleuchtung Marktstand, 2 Tage'), (11, 684, 'Netto: 240,00 EUR'),
              (11, 668, 'USt. 19 %: 45,60 EUR'), (12, 644, 'Gesamt: 285,60 EUR'),
              (11, 612, 'Zahlbar bis 11.11.2026 ohne Abzug.')]
    text = b'\n'.join(b'BT /F1 %d Tf 72 %d Td (%s) Tj ET' % (g, y, t.encode('latin-1')) for g, y, t in zeilen)
    objs = [b'<< /Type /Catalog /Pages 2 0 R >>',
            b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
            b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] '
            b'/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
            b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
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


# ---------------------------------------------------------------- Inhalte

NEWSLETTER_HTML = '''<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8"><title>Herbstmarkt am Rathausplatz</title></head>
<body style="margin:0;padding:0;background:#f3ede4;">
<table role="presentation" width="100%" cellpadding="0" cellspacing="0" style="background:#f3ede4;">
<tr><td align="center" style="padding:24px 12px;">
<table role="presentation" width="600" cellpadding="0" cellspacing="0" style="width:600px;background:#ffffff;border-radius:10px;overflow:hidden;font-family:Helvetica,Arial,sans-serif;color:#3b2a20;">
<tr><td><img src="cid:banner@marktgilde.example" width="600" height="180" alt="Herbstmarkt 2026" style="display:block;border:0;"></td></tr>
<tr><td style="padding:28px 36px 8px 36px;font-size:16px;line-height:1.55;">
<p style="margin:0 0 14px 0;">Hallo Alex,</p>
<p style="margin:0;">der Herbstmarkt kehrt zurück auf den Rathausplatz: Kürbisse, Apfelmost, Handwerk aus der Region und Musik am Brunnen. Sichern Sie sich jetzt Ihren Marktstand.</p>
</td></tr>
<tr><td style="padding:20px 36px 0 36px;">
<h2 style="margin:0 0 6px 0;font-size:19px;color:#a3461b;">Termine</h2>
<p style="margin:0;font-size:15px;line-height:1.55;">Samstag, 14. November, 9 bis 19 Uhr, und Sonntag, 15. November, 11 bis 18 Uhr. Aufbau jeweils ab 7 Uhr, Zufahrt über die Marktstraße.</p>
</td></tr>
<tr><td style="padding:20px 36px 0 36px;">
<h2 style="margin:0 0 6px 0;font-size:19px;color:#a3461b;">Standgebühren</h2>
<p style="margin:0;font-size:15px;line-height:1.55;">3-m-Stand 45 €, 6-m-Stand 80 € für beide Tage, Strom pauschal 12 €. Vereine und Schulen zahlen die Hälfte.</p>
</td></tr>
<tr><td style="padding:20px 36px 0 36px;">
<h2 style="margin:0 0 6px 0;font-size:19px;color:#a3461b;">Anmeldung bis 31. Oktober</h2>
<p style="margin:0;font-size:15px;line-height:1.55;">Die Plätze werden in der Reihenfolge der Anmeldungen vergeben. Die Bestätigung mit Lageplan kommt Anfang November.</p>
</td></tr>
<tr><td align="center" style="padding:28px 36px 32px 36px;">
<a href="https://example.invalid/herbstmarkt/anmeldung" style="display:inline-block;background:#c2571a;color:#ffffff;text-decoration:none;font-weight:bold;font-size:16px;padding:13px 30px;border-radius:24px;">Marktstand sichern</a>
</td></tr>
<tr><td style="background:#faf6f0;padding:18px 36px;font-size:12px;line-height:1.5;color:#8a7a6c;border-top:1px solid #eee3d6;">
Marktgilde Rhein-Sieg e.&nbsp;V. · Rathausplatz 1 · 53721 Siegburg<br>
Sie erhalten diesen Newsletter, weil Sie im letzten Jahr ausgestellt haben.
<a href="https://example.invalid/newsletter/abmelden" style="color:#a3461b;">Abmelden</a>
</td></tr>
</table>
</td></tr></table>
</body></html>
'''

NEWSLETTER_TEXT = '''Hallo Alex,

der Herbstmarkt kehrt zurück auf den Rathausplatz. Sichern Sie sich jetzt Ihren Marktstand.

Termine: Samstag, 14. November, 9–19 Uhr, Sonntag, 15. November, 11–18 Uhr.
Standgebühren: 3-m-Stand 45 €, 6-m-Stand 80 €, Strom pauschal 12 €.
Anmeldung bis 31. Oktober: https://example.invalid/herbstmarkt/anmeldung

Abmelden: https://example.invalid/newsletter/abmelden
'''

BIBLIOTHEK_HTML = '''<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8"></head>
<body style="margin:0;padding:24px;font-family:Helvetica,Arial,sans-serif;color:#1f2d3a;background:#ffffff;">
<h1 style="font-size:22px;color:#1c5d8c;margin:0 0 12px 0;">Willkommen bei Stadtbibliothek online</h1>
<p style="font-size:15px;line-height:1.55;">Ihr Leseausweis ist jetzt mit dem Online-Konto verbunden. Ab sofort können Sie Medien verlängern, vormerken und E-Books ausleihen.</p>
<p style="font-size:15px;line-height:1.55;"><a href="https://example.invalid/bibliothek/konto" style="color:#1c5d8c;">Zum Online-Konto</a></p>
<p style="font-size:13px;color:#6b7a88;">Stadtbibliothek · Öffnungszeiten Di–Fr 10–18 Uhr, Sa 10–14 Uhr</p>
</body></html>
'''


def tag(tage_zurueck, stunde, minute):
    d = HEUTE - datetime.timedelta(days=tage_zurueck)
    return d.replace(hour=stunde, minute=minute)


def mid(name, domain):
    return '<%s%s@%s>' % (KENNUNG, name, domain)


def neue(name, domain, von, an, betreff, datum):
    m = EmailMessage(policy=email.policy.SMTP)
    m['From'] = von
    m['To'] = an
    m['Subject'] = betreff
    m['Date'] = email.utils.format_datetime(datum)
    m['Message-ID'] = mid(name, domain)
    return m


def nachrichten(konto):
    an = 'Alex Sommer <%s>' % konto
    jana = 'Jana Albrecht <jana.albrecht@example.invalid>'
    tom = 'Tom Berger <tom.berger@example.invalid>'
    liste = []  # (Message-ID, gelesen, Datum, Nachricht)

    # a) HTML-Newsletter mit eingebettetem Banner (cid:)
    d = tag(0, 12, 5)
    m = neue('newsletter', 'marktgilde.example', 'Marktgilde Rhein-Sieg <newsletter@marktgilde.example>', an,
             'Herbstmarkt am Rathausplatz – jetzt Marktstand sichern', d)
    m['List-Unsubscribe'] = '<https://example.invalid/newsletter/abmelden>'
    m.set_content(NEWSLETTER_TEXT)
    m.add_alternative(NEWSLETTER_HTML, subtype='html')
    m.get_payload()[1].add_related(banner_png(), maintype='image', subtype='png', cid='<banner@marktgilde.example>',
                                   filename='herbstmarkt-2026.png', disposition='inline')
    liste.append((m['Message-ID'], True, d, m))

    # b) Thread mit drei Nachrichten (Jana an Alex und Tom, Tom antwortet allen, Jana antwortet)
    t1, t2, t3 = tag(1, 18, 20), tag(1, 19, 2), tag(0, 8, 47)
    m1 = neue('thread-1', 'example.invalid', jana, an, 'Marktstand am Samstag', t1)
    m1['Cc'] = tom
    m1.set_content('Hallo Alex, hallo Tom,\n\nsteht der Marktstand am Samstag noch? Ich bringe die Kisten mit '
                   'den Äpfeln und die Kasse mit. Wer kümmert sich um den Pavillon?\n\nViele Grüße\nJana\n')
    m2 = neue('thread-2', 'example.invalid', tom, an, 'Re: Marktstand am Samstag', t2)
    m2['Cc'] = jana
    m2['In-Reply-To'] = m1['Message-ID']
    m2['References'] = m1['Message-ID']
    m2.set_content('Den Pavillon hole ich Freitagabend ab, Aufbau ab 7 Uhr.\nKabeltrommel bringe ich auch mit.\n\n'
                   'Tom\n\n> steht der Marktstand am Samstag noch? Ich bringe die Kisten mit\n'
                   '> den Äpfeln und die Kasse mit. Wer kümmert sich um den Pavillon?\n')
    m3 = neue('thread-3', 'example.invalid', jana, an, 'Re: Marktstand am Samstag', t3)
    m3['Cc'] = tom
    m3['In-Reply-To'] = m2['Message-ID']
    m3['References'] = '%s %s' % (m1['Message-ID'], m2['Message-ID'])
    m3.set_content('Super, dann treffen wir uns um 7 am Brunnen. Ich bringe Kaffee mit!\n\nJana\n\n'
                   '> Den Pavillon hole ich Freitagabend ab, Aufbau ab 7 Uhr.\n')
    for d, m in ((t1, m1), (t2, m2), (t3, m3)):
        liste.append((m['Message-ID'], False, d, m))

    # c) Rechnung mit PDF
    d = tag(0, 10, 14)
    m = neue('rechnung', 'lichtwerk.example', 'Buchhaltung Lichtwerk GmbH <buchhaltung@lichtwerk.example>', an,
             'Rechnung R-2026-1187', d)
    m.set_content('Guten Tag,\n\nanbei erhalten Sie die Rechnung R-2026-1187 über 285,60 € für die Beleuchtung '
                  'Ihres Marktstands.\nZahlbar bis 11.11.2026 ohne Abzug.\n\nMit freundlichen Grüßen\n'
                  'Buchhaltung Lichtwerk GmbH\n')
    m.add_attachment(rechnung_pdf(), maintype='application', subtype='pdf', filename='Rechnung-R-2026-1187.pdf')
    liste.append((m['Message-ID'], False, d, m))

    # d) Fotos mit zwei JPGs
    d = tag(1, 16, 35)
    m = neue('fotos', 'example.invalid', tom, an, 'Fotos vom Stand', d)
    m.set_content('Hi Alex,\n\nhier zwei Fotos vom Probeaufbau. Die Kürbisse links wirken richtig gut, '
                  'oder?\n\nTom\n')
    m.add_attachment(foto_jpg(((96, 140, 180), (92, 120, 64), (214, 120, 40), (186, 60, 40), (240, 236, 226)),
                              'Probeaufbau', 'Stand von vorne'),
                     maintype='image', subtype='jpeg', filename='stand-vorne.jpg')
    m.add_attachment(foto_jpg(((60, 90, 70), (130, 96, 60), (232, 164, 52), (150, 40, 50), (200, 180, 140)),
                              'Kürbisse & Äpfel', 'Auslage links'),
                     maintype='image', subtype='jpeg', filename='kuerbisse-aepfel.jpg')
    liste.append((m['Message-ID'], False, d, m))

    # e) Einladung (iTIP REQUEST), nächste Woche 10:00
    d = tag(0, 9, 30)
    beginn = (HEUTE + datetime.timedelta(days=7)).replace(hour=10, minute=0)
    z = lambda t: t.astimezone(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')  # noqa: E731
    ics = ('BEGIN:VCALENDAR\r\nPRODID:-//Marktgilde//Planung//DE\r\nVERSION:2.0\r\nMETHOD:REQUEST\r\n'
           'BEGIN:VEVENT\r\nUID:%sstandplanung@marktgilde.example\r\nDTSTAMP:%s\r\nDTSTART:%s\r\nDTEND:%s\r\n'
           'SUMMARY:Standplanung\r\nLOCATION:Rathaus\\, Raum 2.14\r\nSEQUENCE:0\r\nSTATUS:CONFIRMED\r\n'
           'ORGANIZER;CN=Lena Hoffmann:mailto:lena.hoffmann@marktgilde.example\r\n'
           'ATTENDEE;CN=Alex Sommer;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:%s\r\n'
           'END:VEVENT\r\nEND:VCALENDAR\r\n'
           % (KENNUNG, z(d), z(beginn), z(beginn + datetime.timedelta(hours=1)), konto))
    m = neue('einladung', 'marktgilde.example', 'Lena Hoffmann <lena.hoffmann@marktgilde.example>', an,
             'Einladung: Standplanung', d)
    m.set_content('Einladung: Standplanung am %s, 10:00 Uhr, Rathaus, Raum 2.14.\nWir verteilen die Plätze '
                  'und klären Strom und Aufbau.\n' % beginn.strftime('%d.%m.%Y'))
    m.add_alternative(ics, subtype='calendar', params={'method': 'REQUEST'})
    liste.append((m['Message-ID'], False, d, m))

    # f) Wochenbericht, reiner Text
    d = tag(2, 7, 0)
    m = neue('wetter', 'wetterdienst.example', 'Wetterdienst Bonn <bericht@wetterdienst.example>', an,
             'Ihr Wochenbericht', d)
    m.set_content('Ihr Wochenbericht für Bonn\n\nMo  12 °C  bewölkt\nDi  14 °C  sonnig\nMi  11 °C  Regen\n'
                  'Do   9 °C  windig\nFr  10 °C  heiter\n\nAm Wochenende trocken, morgens Bodenfrost möglich.\n')
    liste.append((m['Message-ID'], True, d, m))

    # g) Willkommen, HTML einfach
    d = tag(3, 14, 20)
    m = neue('bibliothek', 'stadtbibliothek.example', 'Stadtbibliothek online <konto@stadtbibliothek.example>',
             an, 'Willkommen bei Stadtbibliothek online', d)
    m.set_content('Willkommen bei Stadtbibliothek online. Ihr Leseausweis ist jetzt mit dem Online-Konto '
                  'verbunden.\n')
    m.add_alternative(BIBLIOTHEK_HTML, subtype='html')
    liste.append((m['Message-ID'], False, d, m))

    # h) Protokoll mit Zitat
    d = tag(2, 21, 10)
    m = neue('protokoll', 'beispiel-domain.de', 'Lena Hoffmann <lena.hoffmann@beispiel-domain.de>', an,
             'Protokoll Vereinssitzung', d)
    m.set_content('Hallo zusammen,\n\nhier das Protokoll der Sitzung vom Montag:\n\n1. Herbstmarkt: Stand 6 m, '
                  'Aufbau Samstag 7 Uhr.\n2. Kasse: Jana übernimmt.\n3. Nächste Sitzung: 23. November.\n\n'
                  'Zur Frage von Tom:\n> Brauchen wir dieses Jahr wieder eine Genehmigung für den Strom?\n\n'
                  'Ja, die Marktgilde stellt sie aus, Kosten sind in der Standgebühr enthalten.\n\nLena\n')
    liste.append((m['Message-ID'], False, d, m))

    # i–l) kurze Textmails, damit die sichtbare Liste nur erfundene Mails zeigt
    for name, domain, von, betreff, d, text in (
            ('versand', 'gartenhof.example', 'Gartenhof Versand <versand@gartenhof.example>',
             'Ihre Bestellung ist unterwegs', tag(3, 12, 45),
             'Guten Tag,\n\nIhre Bestellung 48213 (2 Pflanzkübel, 1 Lichterkette) ist unterwegs und kommt '
             'voraussichtlich am Donnerstag.\n\nIhr Gartenhof\n'),
            ('kuchen', 'example.invalid', 'Miriam Koch <miriam.koch@example.invalid>',
             'Kuchenliste für den Stand', tag(3, 10, 30),
             'Hallo Alex,\n\nbisher haben wir Apfelkuchen, Zwetschgendatschi und zwei Bleche Streusel. '
             'Magst du noch einen Käsekuchen beisteuern?\n\nLiebe Grüße\nMiriam\n'),
            ('beitrag', 'tv-siegaue.example', 'Kassenwart TV Siegaue <kasse@tv-siegaue.example>',
             'Mitgliedsbeitrag 2027', tag(3, 8, 5),
             'Liebe Mitglieder,\n\nder Beitrag für 2027 bleibt bei 60 € und wird Mitte Januar eingezogen.\n\n'
             'Sportliche Grüße\nder Kassenwart\n'),
            ('chor', 'cantabile.example', 'Chor Cantabile <info@cantabile.example>',
             'Probe am Donnerstag fällt aus', tag(3, 7, 40),
             'Liebe Sängerinnen und Sänger,\n\ndie Probe am Donnerstag fällt wegen der Herbstferien aus. '
             'Wir sehen uns eine Woche später wie gewohnt um 19:30 Uhr.\n\nEuer Vorstand\n')):
        m = neue(name, domain, von, an, betreff, d)
        m.set_content(text)
        liste.append((m['Message-ID'], False, d, m))
    return liste


# ---------------------------------------------------------------- Ablauf

def vorhandene(c):
    """UID → Message-ID aller Fake-Mails im Posteingang (Suche über die Kennung)."""
    typ, daten = c.uid('SEARCH', None, 'HEADER', 'Message-ID', KENNUNG)
    uids = daten[0].split() if typ == 'OK' and daten[0] else []
    gefunden = {}
    for u in uids:
        typ, teile = c.uid('FETCH', u, '(BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])')
        kopf = b''.join(t[1] for t in teile if isinstance(t, tuple)).decode('ascii', 'replace')
        treffer = re.search(r'<([^>]+)>', kopf)
        if treffer and treffer.group(1).startswith(KENNUNG):
            gefunden[u.decode()] = '<%s>' % treffer.group(1)
    return gefunden


def main():
    modus = sys.argv[1:]
    if modus not in ([], ['--liste'], ['--loeschen']):
        sys.exit('Aufruf: tests/readme-fakemails.py [--liste | --loeschen]')
    konto = konto_adresse()
    c = imap_verbinden(konto)
    try:
        c.select('INBOX', readonly=modus == ['--liste'])
        da = vorhandene(c)
        if modus == ['--liste']:
            print('VORHANDEN %d' % len(da))
            for u, m in sorted(da.items(), key=lambda x: int(x[0])):
                print('  UID %s %s' % (u, m))
            return
        if modus == ['--loeschen']:
            if da:
                c.uid('STORE', ','.join(da), '+FLAGS.SILENT', r'(\Deleted)')
                c.uid('EXPUNGE', ','.join(da))
            print('GELOESCHT %d' % len(da))
            return
        neu = 0
        for message_id, gelesen, datum, m in nachrichten(konto):
            if message_id in da.values():
                continue
            typ, antwort = c.append('INBOX', r'(\Seen)' if gelesen else None, imaplib.Time2Internaldate(datum),
                                    m.as_bytes())
            if typ != 'OK':
                sys.exit('FEHLER  APPEND %s: %s' % (message_id, antwort))
            print('ANGELEGT %s %s' % (message_id, m['Subject']))
            neu += 1
        print('NEU %d, VORHER %d' % (neu, len(da)))
    finally:
        try:
            c.logout()
        except (imaplib.IMAP4.error, OSError):
            pass


if __name__ == '__main__':
    main()
