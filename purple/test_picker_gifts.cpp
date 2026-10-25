#include "base/basic_types.h"
#include "base/weak_ptr.h"
#include "base/weak_qptr.h"
#include "rpl/rpl.h"
#include <QtCore/QCoreApplication>
#include <memory>
#include <variant>
#include <cstdio>

int Checks = 0, Failures = 0;
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::printf("FAIL gifts:%d %s\n", __LINE__, #value); } } while (false)
template <typename T> class object_ptr : public std::unique_ptr<T> {
public:
	using std::unique_ptr<T>::unique_ptr;
	T *data() const { return this->get(); }
};
struct QWidget {};
struct DocumentData {};
struct PhotoData {};
namespace Data { struct FileOrigin {}; }
namespace SendMenu { struct Details {}; }
namespace Window { class SessionController; }
namespace anim { enum class type { Normal }; }
int TransportSends = 0;
struct MTPDpayments_paymentResult { int vupdates() const { return 0; } };
struct MTPDpayments_paymentVerificationNeeded {};
struct MTPpayments_PaymentResult {
	template <typename A, typename B> void match(A, B) const {}
};
struct MTPpayments_SendStarsForm { MTPpayments_SendStarsForm(uint64, int) {} };
uint64 MTP_long(uint64 value) { return value; }
namespace MTP { struct Error { QString type() const { return {}; } }; }
struct FakeRequest {
	template <typename F> FakeRequest &done(F) { return *this; }
	template <typename F> FakeRequest &fail(F) { return *this; }
	void send() { ++TransportSends; }
};
struct FakeApi {
	FakeRequest request(MTPpayments_SendStarsForm) { return {}; }
	void applyUpdates(int) {}
};
namespace Main {
struct Session : base::has_weak_ptr {
	FakeApi apiValue;
	FakeApi &api() { return apiValue; }
};
}
struct PeerData {
	Main::Session *owner = nullptr;
	bool hidden = false;
	Main::Session &session() { return *owner; }
};
namespace Ui {
struct LayerWidget {};
using LayerOptions = int;
struct BoxContent : QObject, base::has_weak_ptr {
	rpl::lifetime life;
	int closes = 0;
	rpl::lifetime &lifetime() { return life; }
	void closeBox() { ++closes; }
};
}
using Layer = std::variant<v::null_t, object_ptr<Ui::BoxContent>, std::unique_ptr<Ui::LayerWidget>>;
namespace ChatHelpers {
struct FileChosen {};
enum class PauseReason { Any };
struct Show {
	virtual ~Show() = default;
	virtual void activate() = 0;
	virtual Main::Session &session() const = 0;
	virtual bool paused(PauseReason) const = 0;
	virtual rpl::producer<> pauseChanged() const = 0;
	virtual rpl::producer<bool> adjustShadowLeft() const = 0;
	virtual SendMenu::Details sendMenuDetails() const = 0;
	virtual bool showMediaPreview(Data::FileOrigin, not_null<DocumentData*>) const = 0;
	virtual bool showMediaPreview(Data::FileOrigin, not_null<PhotoData*>) const = 0;
	virtual void processChosenSticker(FileChosen&&) const = 0;
	virtual Window::SessionController *resolveWindow() const = 0;
	virtual not_null<QWidget*> toastParent() const = 0;
	virtual bool valid() const = 0;
	virtual operator bool() const = 0;
	virtual void showOrHideBoxOrLayer(Layer&&, Ui::LayerOptions, anim::type) const = 0;
};
}
namespace Main { using SessionShow = ChatHelpers::Show; }
namespace Payments {
enum class CheckoutResult { Paid, Cancelled };
struct InvoicePlain {};
struct InvoiceGift { std::function<bool()> pickerCurrent; };
struct InvoiceId { std::variant<InvoicePlain, InvoiceGift> value; };
struct CreditsFormData { InvoiceId id; uint64 formId = 0; int inputInvoice = 0; };
}
namespace Purple {
rpl::event_stream<> Active;
rpl::producer<> ActiveChanges() { return Active.events(); }
bool PickerVisible(not_null<PeerData*> peer) { return !peer->hidden; }
std::function<bool()> PickerAction(not_null<PeerData*> peer, rpl::lifetime &lifetime, Fn<void()> revoked = nullptr);
void WatchPickerChild(not_null<Ui::BoxContent*> box, not_null<PeerData*> peer);
#include "gift-owned.inc"
#include "gift-scope.inc"
#include "gift-current.inc"
}
struct FakeShow final : ChatHelpers::Show {
	Main::Session *owner = nullptr;
	mutable QWidget widget;
	mutable std::vector<object_ptr<Ui::BoxContent>> boxes;
	bool alive = true;
	void activate() override {}
	Main::Session &session() const override { return *owner; }
	bool paused(ChatHelpers::PauseReason) const override { return false; }
	rpl::producer<> pauseChanged() const override { return rpl::never<>(); }
	rpl::producer<bool> adjustShadowLeft() const override { return rpl::single(false); }
	SendMenu::Details sendMenuDetails() const override { return {}; }
	bool showMediaPreview(Data::FileOrigin, not_null<DocumentData*>) const override { return true; }
	bool showMediaPreview(Data::FileOrigin, not_null<PhotoData*>) const override { return true; }
	void processChosenSticker(ChatHelpers::FileChosen&&) const override {}
	Window::SessionController *resolveWindow() const override { return nullptr; }
	not_null<QWidget*> toastParent() const override { return &widget; }
	bool valid() const override { return alive; }
	operator bool() const override { return valid(); }
	void showOrHideBoxOrLayer(Layer &&layer, Ui::LayerOptions, anim::type) const override {
		if (auto box = std::get_if<object_ptr<Ui::BoxContent>>(&layer)) boxes.push_back(std::move(*box));
	}
};
namespace FinalProbe {
#include "gift-final-stars.inc"
}
namespace GiftProbe {
struct GiftTypeStars {};
struct Details { bool byStars = false; std::variant<int, GiftTypeStars> descriptor = 0; };
struct Credits { void load(bool) {} };
struct Session { Credits value; Credits &credits() { return value; } };
struct Window { Session value; Session &session() { return value; } void showPeerHistory(PeerData*) {} };
struct State { int media = 0; };
int Gifts = 0, HiddenGifts = 0;
template <typename F> void SendGift(Window*, PeerData *peer, void*, const Details&, F, std::shared_ptr<ChatHelpers::Show>) { ++Gifts; HiddenGifts += peer->hidden; }
void ShowSentToast(Window*, const std::variant<int, GiftTypeStars>&, const Details&) {}
void Submit(PeerData *peer, std::shared_ptr<ChatHelpers::Show> show) {
	Ui::BoxContent owned;
	State stateValue;
	Window windowValue;
	Details details;
	auto box = &owned;
	auto state = &stateValue;
	auto window = &windowValue;
	void *api = nullptr;
	auto submit = [=] {
#include "gift-submit.inc"
	};
	submit();
}
}
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto session = std::make_unique<Main::Session>();
	auto peer = PeerData{session.get()};
	auto plain = std::make_shared<FakeShow>();
	plain->owner = session.get();
	auto scope = std::make_shared<Purple::RecipientGiftShow>(plain, &peer);
	CHECK(!Purple::PickerGiftCurrent(plain));
	CHECK(bool(Purple::PickerGiftCurrent(scope)));
	auto invoice = Payments::InvoiceId{Payments::InvoiceGift{Purple::PickerGiftCurrent(scope)}};
	auto panelLife = rpl::lifetime();
	auto panelClosed = 0;
	Purple::WatchPickerPayment(invoice, [&] { ++panelClosed; }, panelLife);
	for (auto i = 0; i != 4; ++i) scope->showOrHideBoxOrLayer(object_ptr<Ui::BoxContent>(new Ui::BoxContent()), 0, {});
	CHECK(scope->valid()); CHECK(plain->boxes.size() == 4); CHECK(Purple::PickerPaymentAllowed(invoice));
	auto form = std::make_shared<Payments::CreditsFormData>(); form->id = invoice;
	auto denied = 0;
	const auto finalSubmit = [&] { FinalProbe::SendStarsForm(session.get(), form, [&](std::optional<QString> error) { if (error) ++denied; }); };
	finalSubmit(); CHECK(TransportSends == 1); CHECK(denied == 0);
	GiftProbe::Submit(&peer, scope); CHECK(GiftProbe::Gifts == 1);
	peer.hidden = true;
	Purple::Active.fire({});
	CHECK(!scope->valid()); CHECK(!Purple::PickerPaymentAllowed(invoice)); CHECK(panelClosed == 1);
	finalSubmit(); CHECK(TransportSends == 1); CHECK(denied == 1);
	for (const auto &box : plain->boxes) CHECK(box->closes == 1);
	GiftProbe::Submit(&peer, scope); CHECK(GiftProbe::Gifts == 1); CHECK(GiftProbe::HiddenGifts == 0);
	scope->showOrHideBoxOrLayer(object_ptr<Ui::BoxContent>(new Ui::BoxContent()), 0, {}); CHECK(plain->boxes.size() == 4);
	peer.hidden = false;
	Purple::Active.fire({});
	CHECK(!scope->valid()); CHECK(!Purple::PickerPaymentAllowed(invoice));
	finalSubmit(); CHECK(TransportSends == 1); CHECK(denied == 2);
	GiftProbe::Submit(&peer, scope); CHECK(GiftProbe::Gifts == 1);
	GiftProbe::Submit(&peer, plain); CHECK(GiftProbe::Gifts == 2);
	peer.hidden = true;
	GiftProbe::Submit(&peer, plain); CHECK(GiftProbe::Gifts == 3); CHECK(GiftProbe::HiddenGifts == 1);
	CHECK(Purple::PickerPaymentAllowed({Payments::InvoicePlain{}}));
	CHECK(Purple::PickerPaymentAllowed({Payments::InvoiceGift{}}));
	peer.hidden = false;
	auto newScope = std::make_shared<Purple::RecipientGiftShow>(plain, &peer);
	CHECK(newScope->valid());
	session.reset();
	CHECK(!newScope->valid());
	std::printf("gift descendants %d checks, %d failures; reviewer hidden submit: 0 dispatches\n", Checks, Failures);
	return Failures ? 1 : 0;
}
