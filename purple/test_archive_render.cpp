#include "base/integration.h"
#include "lang/lang_keys.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_v2_ui.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/abstract_button.h"
#include "ui/effects/animations.h"
#include "ui/emoji_config.h"
#include "ui/integration.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_direction.h"
#include "ui/style/style_core_palette.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QResource>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QTextEdit>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <cstdio>
#include <cstdlib>

namespace crl {
rpl::producer<> on_main_update_requests() { return rpl::never<>(); }
}
namespace Lang::details {
QString Current(ushort key) {
	auto result = GetOriginalValue(key);
	if (!st::rtl()) return result;
	const auto exact = std::initializer_list<std::pair<QStringView, QStringView>>{
		{ u"View Edit History", u"سجل الأرشيف" },
		{ u"Message history", u"سجل الرسالة" },
		{ u"Deleted messages", u"الرسائل المحذوفة" },
		{ u"Deleted", u"حُذفت" },
		{ u"Expired", u"انتهت الصلاحية" },
		{ u"Consumed", u"شوهدت لمرة واحدة" },
		{ u"Edited", u"عُدلت" },
		{ u"Current", u"الحالية" },
		{ u"Close", u"إغلاق" },
		{ u"Older versions", u"الإصدارات الأقدم" },
		{ u"Older messages", u"الرسائل الأقدم" },
		{ u"Filter loaded messages", u"تصفية الرسائل المحمّلة" },
		{ u"No matching loaded messages", u"لا توجد رسائل مطابقة" },
		{ u"Unknown chat", u"محادثة غير معروفة" },
		{ u"Attachment", u"مرفق" },
		{ u"Saving…", u"جارٍ الحفظ…" },
		{ u"Archive storage", u"تخزين الأرشيف" },
		{ u"Messages and files", u"الرسائل والملفات" },
		{ u"View once and timed media", u"وسائط العرض مرة واحدة والمؤقتة" },
		{ u"Waiting for deletion", u"بانتظار الحذف" },
		{ u"Storage overhead", u"مساحة التخزين الإضافية" },
		{ u"Clean up", u"تنظيف الأرشيف" },
		{ u"Clear edit history…", u"مسح سجل التعديلات…" },
		{ u"Empty deleted messages…", u"إفراغ الرسائل المحذوفة…" },
		{ u"Previous archive", u"الأرشيف السابق" },
		{ u"Size on disk", u"الحجم على القرص" },
		{ u"Delete previous archive…", u"حذف الأرشيف السابق…" },
		{ u"Delete previous archive?", u"هل تريد حذف الأرشيف السابق؟" },
		{ u"This permanently removes this account’s previous local archive.", u"سيُحذف الأرشيف المحلي السابق لهذا الحساب نهائيًا." },
		{ u"Delete (Y)", u"حذف (Y)" },
		{ u"Cancel (Esc)", u"إلغاء (Esc)" },
	};
	for (const auto &[english, arabic] : exact) if (result == english) return arabic.toString();
	if (result.contains(u"earlier"_q, Qt::CaseInsensitive) || result.contains(u"Earlier"_q)) {
		return u"لم يظهر النص السابق على هذا الجهاز."_q;
	}
	for (const auto &[english, arabic] : {
			std::pair<QStringView, QStringView>{ u"Used", u"المستخدم" }, { u" of ", u" من " },
			{ u"Limit for each ordinary file:", u"الحد لكل ملف عادي:" },
			{ u"Save ", u"حفظ " }, { u"Not saved", u"لم يُحفظ" },
			{ u"October", u"أكتوبر" }, { u"Oct", u"أكتوبر" }, { u" at ", u" الساعة " }, { u"Today", u"اليوم" }, { u"Yesterday", u"أمس" } }) {
		result.replace(english.toString(), arabic.toString());
	}
	return result;
}
rpl::producer<QString> Value(ushort key) { return rpl::single(Current(key)); }
}
namespace Data {
UserData *Session::userLoaded(UserId) const { std::abort(); }
// The fixture uses the real rich-message parser without a Telegram session.
not_null<PeerData*> Session::processChat(const MTPChat &) { std::abort(); }
not_null<PhotoData*> Session::processPhoto(const MTPPhoto &) { std::abort(); }
not_null<DocumentData*> Session::processDocument(const MTPDocument &, const MTPVector<MTPDocument> *) { std::abort(); }
}
namespace Main {
UserId Session::userId() const { std::abort(); }
}
const QString &PeerData::name() const { std::abort(); }
namespace Purple {
bool ModalKeysEnabled() { return true; }
QString CopyTextAsMarkdown(const TextForMimeData &text) { return text.rich.text; }
}

namespace {
using namespace Purple::ArchiveV2;

class BaseIntegration final : public base::Integration {
public:
	using base::Integration::Integration;
	void enterFromEventLoop(FnMut<void()> &&method) override { method(); }
	bool logSkipDebug() override { return true; }
	void logMessageDebug(const QString &) override {}
	void logMessage(const QString &) override {}
	void logAssertionViolation(const QString &message) override {
		std::fprintf(stderr, "archive render assertion: %s\n", qPrintable(message));
	}
};

class Integration final : public Ui::Integration {
public:
	void postponeCall(FnMut<void()> &&callable) override {
		QTimer::singleShot(0, qApp, std::move(callable));
	}
	void registerLeaveSubscription(not_null<QWidget*>) override {}
	void unregisterLeaveSubscription(not_null<QWidget*>) override {}
	QString emojiCacheFolder() override { return qEnvironmentVariable("XDG_CACHE_HOME") + u"/emoji"_q; }
	QString fontsCacheFolder() override { return {}; }
	QString openglCheckFilePath() override { return {}; }
	QString angleBackendFilePath() override { return {}; }
	void touchCounterIncrement() override {}
	int touchCounterNow() override { return 0; }
};

class Canvas final : public QWidget {
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter(this).fillRect(rect(), st::windowBg->c);
	}
};

QByteArray Read(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void ApplyNightPalette(const QString &path) {
	const auto source = QString::fromUtf8(Read(path));
	auto palette = style::palette();
	const auto pattern = QRegularExpression(
		QStringLiteral(R"(^([a-zA-Z][a-zA-Z0-9_]*):\s*([#a-zA-Z0-9_]+);)"),
		QRegularExpression::MultilineOption);
	for (const auto &match : pattern.globalMatch(source)) {
		const auto key = match.captured(1).toLatin1();
		const auto value = match.captured(2);
		if (value.startsWith(u'#')) {
			const auto rgb = value.mid(1, 6).toUInt(nullptr, 16);
			const auto alpha = value.size() == 9 ? value.mid(7).toUInt(nullptr, 16) : 255;
			palette.setColor(QLatin1String(key), rgb >> 16, rgb >> 8, rgb, alpha);
		} else {
			palette.setColor(QLatin1String(key), QLatin1String(value.toLatin1()));
		}
	}
	palette.finalize();
	style::main_palette::apply(palette);
}

bool Render(const QString &out, int width, const char *name,
		Fn<void(not_null<Ui::GenericBox*>)> fill,
		Fn<bool(not_null<Ui::GenericBox*>)> exercise = {}) {
	auto window = Canvas();
	window.resize(width, 720);
	auto layers = Ui::LayerStackWidget(&window, {});
	layers.setGeometry(window.rect());
	window.show();
	window.activateWindow();
	auto content = Box<Ui::GenericBox>(std::move(fill));
	const auto box = QPointer<Ui::GenericBox>(content.data());
	layers.showBox(std::move(content), Ui::LayerOption::CloseOther, anim::type::instant);
	qApp->processEvents();
	const auto image = window.grab().toImage();
	if (!box || !image.save(out + '/' + QString::fromLatin1(name) + u".png"_q)) return false;
	const auto geometry = box->mapTo(&window, QPoint(0, 0));
	if (geometry.x() < 0 || geometry.x() + box->width() > width) {
		std::fprintf(stderr, "archive box %s is clipped at width %d\n", name, width);
		return false;
	}
	const auto layout = box->verticalLayout();
	for (auto child : layout->children()) {
		const auto row = dynamic_cast<QWidget*>(child);
		if (!row || !row->isVisible()) continue;
		const auto left = row->mapTo(box, QPoint()).x();
		if (left < 0 || left + row->width() > static_cast<QWidget*>(box.data())->width()) {
			std::fprintf(stderr, "archive box %s has a clipped row at width %d (x=%d, row=%d, box=%d, layout=%d)\n",
				name, width, left, row->width(), static_cast<QWidget*>(box.data())->width(), layout->width());
			return false;
		}
	}
	return !exercise || exercise(box.data());
}

void Key(QWidget *widget, int code, bool repeat = false) {
	if (!widget) return;
	auto press = QKeyEvent(QEvent::KeyPress, code, Qt::NoModifier, {}, repeat);
	auto release = QKeyEvent(QEvent::KeyRelease, code, Qt::NoModifier, {}, repeat);
	const auto alive = QPointer(widget);
	QApplication::sendEvent(widget, &press);
	if (alive) QApplication::sendEvent(alive, &release);
	qApp->processEvents();
}

void Click(QWidget *widget) {
	const auto local = widget->rect().center();
	const auto global = widget->mapToGlobal(local);
	auto move = QMouseEvent(QEvent::MouseMove, local, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	auto press = QMouseEvent(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	auto release = QMouseEvent(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	for (auto event : { &move, &press, &release }) QApplication::sendEvent(widget, event);
	qApp->processEvents();
}

Ui::AbstractButton *Button(not_null<Ui::GenericBox*> box, QString text) {
	for (auto child : box->window()->findChildren<QWidget*>()) {
		if (auto button = dynamic_cast<Ui::AbstractButton*>(child); button && button->isVisible()
			&& button->accessibilityName().contains(text)) return button;
	}
	return nullptr;
}

bool Check(bool ok, const char *check) {
	if (!ok) std::fprintf(stderr, "archive render fixture: %s\n", check);
	return ok;
}
}

int main(int argc, char **argv) {
	if (argc != 5) return 2;
	auto app = QApplication(argc, argv);
	auto base = BaseIntegration(argc, argv);
	base::Integration::Set(&base);
	auto integration = Integration();
	Ui::Integration::Set(&integration);
	auto animations = Ui::Animations::Manager();
	const auto width = QByteArray(argv[3]).toInt();
	style::SetRightToLeft(QByteArray(argv[4]) == "rtl");
	QFontDatabase::addApplicationFont(u"Telegram/lib_ui/fonts/OpenSans-Regular.ttf"_q);
	QFontDatabase::addApplicationFont(u"Telegram/lib_ui/fonts/OpenSans-SemiBold.ttf"_q);
	style::SetScale(100);
	style::StartManager(100);
	style::main_palette::reset();
	if (!Check(QResource::registerResource(qEnvironmentVariable("PURPLE_ARCHIVE_RENDER_RESOURCES")),
			"load Telegram fixture resources")) return 1;
	Ui::Emoji::Init();
	const auto clearEmoji = gsl::finally([] { Ui::Emoji::Clear(); });
	if (*argv[2]) ApplyNightPalette(QString::fromLocal8Bit(argv[2]));
	const auto directory = QString::fromLocal8Bit(argv[1]);
	const auto current = [] { return true; };
	const auto decode = [](const QByteArray &bytes) {
		return TextForMimeData::Simple(QString::fromUtf8(bytes));
	};
	const auto rtl = st::rtl();
	if (rtl) QLocale::setDefault(QLocale(QLocale::Arabic, QLocale::Egypt));
	const auto words = rtl
		? u"هذه رسالة تجريبية طويلة تحتوي على تفاصيل الاجتماع وموعده. أضيفت ملاحظات جديدة عن المكان والمستندات المرفقة. يمكن مراجعة النص الكامل هنا قبل تنزيل الملف."_q
		: u"The meeting moved to Thursday at noon. Bring the updated agenda and the notes from last week. The attached draft has the full schedule and the remaining questions for the group."_q;
	const auto before = int64(1760000000);
	const auto filename = rtl ? u"جدول الاجتماع.pdf"_q : u"Meeting agenda.pdf"_q;
	const auto descriptor = QJsonDocument(QJsonObject{ { u"name"_q, filename } }).toJson(QJsonDocument::Compact);
	const auto photo = MediaStatus{ .kind = MediaKind::Photo };
	const auto video = MediaStatus{ .description = R"({"mime":"video/webm"})", .kind = MediaKind::Video };
	auto history = HistoryPage{ MessageStatus::Deleted, true, {}, 6, before + 360 };
	history.rows.push_back({ 9, { before + 300, words.toUtf8() }, {
		{ 0, descriptor, 32768, {} }, { 1, descriptor, 0, {}, MediaKind::Photo },
		{ 2, descriptor, 0, u"expired"_q, MediaKind::Video } } });
	for (auto seq = 8; seq >= 6; --seq) {
		history.rows.push_back({ seq, { before + seq * 18, (words + u" "_q + words).toUtf8() }, {} });
	}
	using Flags = MTPDmessage::Flag;
	const auto bot = MTP_peerUser(MTP_long(17));
	const auto lead = u"Fixture echo."_q;
	const auto tail = u"Marker returned. 🟣"_q;
	const auto paragraph = [](const auto &text) { return MTP_pageBlockParagraph(text); };
	const MTPRichMessage rich = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()), MTP_vector<MTPPageBlock>({
		paragraph(MTP_textConcat(MTP_vector<MTPRichText>({
			MTP_textBold(MTP_textPlain(MTP_string(lead))), MTP_textPlain(MTP_string(u" "_q)),
			MTP_textCustomEmoji(MTP_long(321), MTP_string(u"✅"_q)),
		}))),
		paragraph(MTP_textPlain(MTP_string(tail))),
	}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	const auto makeWire = [=](const QString &text, const MTPVector<MTPMessageEntity> &entities,
			const MTPRichMessage *blocks) {
		const auto flags = Flags::f_from_id | (blocks ? Flags::f_rich_message : Flags::f_entities);
		return MTP_message(MTP_flags(flags), MTP_int(290), bot, {}, {}, bot, {}, {}, {}, {}, {}, {},
			MTP_int(int(before)), MTP_string(text), {}, {}, entities, // media, reply markup, entities
			{}, {}, {}, {}, {}, {}, {}, {}, // views through restrictions
			{}, {}, {}, {}, {}, {}, {}, {}, {}, blocks ? *blocks : MTPRichMessage());
	};
	const auto wire = makeWire({}, {}, &rich);
	auto captured = CapturedEvent{ .identity = { { Purple::PeerTokenKind::User, 17 }, 290 }, .kind = ChatClass::Bot };
	Purple::CaptureArchiveBody(nullptr, wire.c_message(), captured);
	const auto echo = Purple::ArchivedText(captured.body).rich;
	const auto expected = lead + u" ✅\n"_q + tail;
	auto success = Check(!wire.c_message().is_out() && qs(wire.c_message().vmessage()).isEmpty()
		&& captured.attachments.empty() && echo.text == expected
		&& echo.entities == EntitiesInText{
			{ EntityType::Bold, 0, int(lead.size()) },
			{ EntityType::CustomEmoji, int(lead.size()) + 1, 1, u"321"_q },
		}, "incoming rich bot reply retains both text paragraphs, bold and custom emoji");
	const auto echoPage = TimelinePage{ { {
		.identity = captured.identity, .status = MessageStatus::Live,
		.observedAt = before, .latest = StoredBody{ before, captured.body }, .kind = ChatClass::Bot,
	} }, {} };
	success &= Render(directory, width, "rich-echo", [&](not_null<Ui::GenericBox*> box) {
		FillTimelineBox(box, echoPage, false, Purple::ArchivedText, current, {});
	}, [&](not_null<Ui::GenericBox*> box) {
		return Check(Button(box, expected) && !Button(box, tr::lng_purple_archive_v2_media_preview(tr::now)),
			"the native timeline uses the rich bot reply instead of Attachment");
	});
	success &= Render(directory, width, "rich-echo-history", [&](not_null<Ui::GenericBox*> box) {
		const auto page = HistoryPage{ .rows = { { 1, { before, captured.body } } } };
		FillHistoryBox(box, page, Purple::ArchivedText, current, {});
	}, [&](not_null<Ui::GenericBox*> box) {
		for (const auto child : box->findChildren<QWidget*>()) {
			if (const auto label = dynamic_cast<Ui::FlatLabel*>(child); label && label->isVisible()
				&& label->accessibilityName().contains(lead) && label->accessibilityName().contains(tail)) return true;
		}
		return Check(false, "native single-message History displays both paragraphs of the rich bot reply");
	});
	const auto ordinary = makeWire(lead + u" ✅"_q, MTP_vector<MTPMessageEntity>({
		MTP_messageEntityBold(MTP_int(0), MTP_int(int(lead.size()))),
		MTP_messageEntityCustomEmoji(MTP_int(int(lead.size()) + 1), MTP_int(1), MTP_long(321)),
	}), nullptr);
	auto ordinaryBody = CapturedEvent();
	Purple::CaptureArchiveBody(nullptr, ordinary.c_message(), ordinaryBody);
	success &= Check(Purple::ArchivedText(ordinaryBody.body).rich == TextWithEntities{ lead + u" ✅"_q, echo.entities },
		"ordinary wire text and emoji entities are unchanged");
	const MTPRichMessage empty = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()), MTP_vector<MTPPageBlock>(),
		MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	const auto fallback = makeWire(lead, {}, &empty);
	auto fallbackBody = CapturedEvent();
	Purple::CaptureArchiveBody(nullptr, fallback.c_message(), fallbackBody);
	success &= Check(Purple::ArchivedText(fallbackBody.body).rich.text == lead, "empty rich blocks retain wire text");
	auto exports = 0;
	auto loads = 0;
	success &= Check(ArchiveFileName(photo, before).startsWith(u"photo-")
		&& ArchiveFileName(photo, before).endsWith(u".jpg")
		&& ArchiveFileName(video, before).endsWith(u".webm")
		&& ArchiveFileName({ 0, descriptor }, before) == filename, "original, MIME and kind filename rules");
	success &= Render(directory, width, "history", [&](not_null<Ui::GenericBox*> box) {
		FillHistoryBox(box, history, decode, current, [&](int64 cursor, auto done) {
			if (cursor == 6) ++loads;
			done(HistoryPage{ MessageStatus::Deleted, true, { { 5, { before, words.toUtf8() }, {} } }, 4 });
		}, {
			.saving = [](int64 seq, int64 ordinal) { return seq == 9 && ordinal == 1; },
			.openSaved = [&](int64 sequence, int64 ordinal, QString name) {
				if (sequence == 9 && ordinal == 0 && name == filename) ++exports;
			},
		});
	}, [&](not_null<Ui::GenericBox*> box) {
		auto saved = Button(box, filename);
		auto older = Button(box, tr::lng_purple_archive_older(tr::now));
		if (!Check(saved && older, "history export and paging controls")) return false;
		box->scrollToY(32);
		const auto y = box->scrollTop();
		const auto rows = box->rowsCount();
		saved->setFocus();
		saved->clicked(Qt::NoModifier, Qt::LeftButton);
		auto focus = QApplication::focusWidget();
		older->clicked(Qt::NoModifier, Qt::LeftButton);
		return Check(exports == 1 && loads == 1 && y > 0 && box->scrollTop() == y
			&& box->rowsCount() > rows && focus == QApplication::focusWidget(), "export target, history scroll and focus survive append");
	});
	for (const auto allChats : { true, false }) {
		auto timeline = TimelinePage{ {}, TimelineCursor() };
		for (auto i = 0; i != 9; ++i) {
			timeline.rows.push_back({ { {}, uint64(123 + i) }, i == 1 ? MessageStatus::Expired : MessageStatus::Deleted,
				before + 300 - i * 60, StoredBody{ before, (words + u' ' + words).toUtf8() }, {} });
		}
		auto opened = uint64(0);
		success &= Render(directory, width, allChats ? "deleted-all" : "deleted-chat", [&](not_null<Ui::GenericBox*> box) {
			FillTimelineBox(box, timeline, true, decode, current, [&](TimelineCursor, auto done) {
				done(TimelinePage{ { { { {}, 999 }, MessageStatus::Consumed, before,
					StoredBody{ before, words.toUtf8() }, {} } }, TimelineCursor() });
			}, [=](const TimelineEntry &item) {
				const auto group = rtl ? u"فريق التصميم"_q : u"Design team"_q;
				if (allChats && item.identity.message == 124) return QString();
				const auto sender = item.identity.message % 2 ? (rtl ? u"نور"_q : u"Morgan"_q) : (rtl ? u"علي"_q : u"Taylor"_q);
				return (allChats && item.identity.message % 3 == 0 ? (rtl ? u"مجموعة الدراسة"_q : u"Study group"_q) : group)
					+ u" · "_q + sender;
			}, [&](MessageIdentity item) { opened = item.message; });
		}, [&](not_null<Ui::GenericBox*> box) {
			const auto first = QApplication::focusWidget();
			const auto older = Button(box, tr::lng_purple_archive_v2_older_messages(tr::now));
			if (!Check(first && older && dynamic_cast<Ui::AbstractButton*>(first), "native timeline cursor")) return false;
			Key(first, Qt::Key_Return, true);
			if (!Check(opened == 0, "held Return stays inert")) return false;
			Key(first, Qt::Key_Return);
			if (!Check(opened == 123, "timeline Return uses focused message")) return false;
			Key(first, Qt::Key_J);
			const auto second = QApplication::focusWidget();
			Key(second, Qt::Key_Return);
			if (!Check(opened == 124, "j moves to the expired placeholder's archived history")) return false;
			Key(second, Qt::Key_Slash);
			const auto edit = dynamic_cast<QTextEdit*>(QApplication::focusWidget());
			if (!Check(edit, "slash focuses native filter")) return false;
			edit->insertPlainText(u"no fixture matches"_q);
			Key(edit, Qt::Key_Escape);
			if (!Check(edit->toPlainText().isEmpty() && QApplication::focusWidget() == second,
				"Escape clears filter and restores the same message")) return false;
			const auto y = box->scrollTop();
			const auto rows = box->rowsCount();
			older->clicked(Qt::NoModifier, Qt::LeftButton);
			if (!Check(box->scrollTop() == y && box->rowsCount() == rows + 1 && QApplication::focusWidget() == second,
				"timeline appends without resetting focus or scroll")) return false;
			opened = 0;
			Click(first);
			return Check(opened == 123, "timeline mouse and Return use the same message");
		});
	}
	success &= Render(directory, width, "storage", [&](not_null<Ui::GenericBox*> box) {
		FillStorageBox(box, StorageSummary{ { 90 * kMiB, 8 * kMiB, 0, 0, 65 * kMiB, 24 * kMiB },
			2 * kMiB, 512 * kMiB, 256 * kMiB, 16 * kMiB, 64 * kMiB }, 24 * kMiB,
			current, [](CleanupKind) {}, [] {});
	});
	auto confirmed = 0;
	success &= Render(directory, width, "previous-confirm", [&](not_null<Ui::GenericBox*> box) {
		FillPreviousArchiveConfirm(box, rtl ? u"حساب تجريبي"_q : u"Fixture account"_q, 24 * kMiB,
			current, [&] { ++confirmed; });
	}, [&](not_null<Ui::GenericBox*> box) {
		const auto cancel = Button(box, tr::lng_purple_archive_v2_confirm_cancel(tr::now));
		if (!Check(cancel && QApplication::focusWidget() == cancel, "confirmation initially focuses Cancel")) return false;
		Key(cancel, Qt::Key_Y, true);
		if (!Check(confirmed == 0, "repeat cannot confirm")) return false;
		Key(cancel, Qt::Key_Y);
		return Check(confirmed == 1, "confirmation receives one intentional press");
	});
	style::StopManager();
	return success ? 0 : 1;
}
