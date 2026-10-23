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
#   tests/testdaten.py --last N   #352: N Lastmails per IMAP-APPEND (kein Versand) in den Ordner
#                                 „Revolution-Last“ von privat@ (fehlende ergänzen); letzte
#                                 Zeile „LAST <UID>/<voller Ordnername> <Anzahl>“
#   tests/testdaten.py --flags KONTO
#                                 #353, nur lesend, unabhängig von Camel: Flags der Mails im Ordner
#                                 Revolution-Test direkt vom Server (IMAP EXAMINE + UID FETCH FLAGS
#                                 bzw. Graph GET isRead/importance/flag); Zeilen „ORDNER <UID>/<Name>“
#                                 und „FLAGS <gelesen 0|1> <markiert 0|1> <Betreff>“
#   tests/testdaten.py --server KONTO AKTION
#                                 #353, schreibend nur in Testkonten: serverseitig an der Einladung
#                                 in Revolution-Test gelesen|ungelesen|markiert|unmarkiert (IMAP UID
#                                 STORE bzw. Graph PATCH), neu (IMAP APPEND einer Live-Mail) oder
#                                 weg (Live-Mails löschen: UID STORE \Deleted + EXPUNGE)
#   tests/testdaten.py --ungelesen KONTO ORDNER [--soll DATEI]
#                                 #353: Zeile „UNGELESEN <Anzahl> <UID …>“ (IMAP EXAMINE + UID SEARCH
#                                 UNSEEN, nur lesend); mit --soll (Datei mit einer solchen Zeile)
#                                 stellt es genau diesen Stand per UID STORE ±\Seen her (Testkonten)
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import ctypes
import datetime
import imaplib
import json
import re
import email.header
import email.policy
import email.utils
import os
import urllib.request
import struct
import sys
import threading
import time
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


def last(k, anzahl, dry):
    """#352: fehlende Lastmails 1..anzahl per APPEND in LAST_ORDNER (idempotent)."""
    nur_testkonten([k.adresse])
    folder = ordner_holen(k.store, LAST_ORDNER, not dry)
    if folder is None:
        print('  %s: Ordner %s anlegen, %d Lastmails einspielen' % (k.adresse, LAST_ORDNER, anzahl))
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


def server(k, aktion, ziel=ORDNER):
    """#353: serverseitige Änderung in einem Testkonto, während die App läuft; neu/weg auch im
    Posteingang (ziel INBOX, Beleg für IDLE)."""
    nur_testkonten([k.adresse])
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        name, _ = imap_ordner(c, ziel)
        c.select('"%s"' % name)
        if aktion == 'neu':
            m = EmailMessage(policy=email.policy.SMTP)
            m.set_content('Live-Mail für #353 (APPEND, wird danach gelöscht).\n')
            m['Subject'] = '%s %s' % (LIVE, datetime.datetime.now().strftime('%H:%M:%S'))
            m['From'] = k.adresse
            m['To'] = k.adresse
            m['Date'] = email.utils.formatdate(localtime=True)
            print(c.append('"%s"' % name, None, None, m.as_bytes()))
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


def main():
    ap = argparse.ArgumentParser(description='Testdaten für Revolution (#344)')
    ap.add_argument('--dry-run', action='store_true', help='nur anzeigen, nichts senden/schreiben')
    ap.add_argument('--pruefen', action='store_true', help='nur lesend prüfen')
    ap.add_argument('--ordner', action='store_true', help='nur lesend: Ordnerliste aller Konten')
    ap.add_argument('--last', type=int, metavar='N',
                    help='N Lastmails per APPEND in %s von %s (#352)' % (LAST_ORDNER, LAST_KONTO))
    ap.add_argument('--flags', metavar='KONTO', help='#353: nur lesend Flags in %s vom Server' % ORDNER)
    ap.add_argument('--ungelesen', nargs=2, metavar=('KONTO', 'ORDNER'),
                    help='#353: ungelesene UIDs (nur lesend); mit --soll DATEI wiederherstellen')
    ap.add_argument('--soll', metavar='DATEI', help='#353: Ausgabe von --ungelesen als Sollstand')
    ap.add_argument('--server', nargs='+', metavar='KONTO AKTION [INBOX]',
                    help='#353: serverseitig gelesen|ungelesen|markiert|unmarkiert|neu|weg [im Posteingang]')
    args = ap.parse_args()

    if args.ungelesen:
        adresse = args.ungelesen[0].lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return ungelesen(k, args.ungelesen[1], args.soll)
    if args.flags or args.server:
        adresse = (args.flags or args.server[0]).lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        if args.server and (len(args.server) not in (2, 3) or args.server[2:] not in ([], ['INBOX'])):
            ap.error('--server KONTO AKTION [INBOX]')
        return flags(k) if args.flags else server(k, *args.server[1:])

    os.makedirs(os.path.join(CACHE, 'data'), exist_ok=True)
    os.makedirs(os.path.join(CACHE, 'cache'), exist_ok=True)
    Camel.init(CACHE, False)
    Camel.provider_init()
    EDataServer.SourceCamel.register_types()
    registry = EDataServer.SourceRegistry.new_sync(None)
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
        n = last(k, args.last, args.dry_run)
        k.store.disconnect_sync(True, None)
        return n
    for k in konten.values():
        store_oeffnen(session, k)

    if args.pruefen:
        return 1 if pruefen(konten) else 0
    n = senden(session, konten, args.dry_run)
    if n and not args.dry_run:
        print('  warte auf Zustellung …')
        warten(konten)
    ablegen(konten, args.dry_run)
    for k in konten.values():
        k.store.disconnect_sync(True, None)
    return 0


if __name__ == '__main__':
    sys.exit(main())
