#pragma once
#include "purple/purple_archive_envelope.h"
#include <map>
#include <memory>

namespace Purple {

class DesktopArchiveCrypto final {
  public:
	DesktopArchiveCrypto();
	void Revoke();
	[[nodiscard]] bool AddKey(const QString &id, const QByteArray &key);
	[[nodiscard]] ArchiveCrypto callbacks() const;
	[[nodiscard]] std::function<std::optional<QByteArray>(const QByteArray &)> indexHmac(
		const QString &id) const;
	[[nodiscard]] static std::optional<QByteArray> Random(int size);
	[[nodiscard]] static std::optional<QByteArray> Derive(
		const QByteArray &input, const ArchiveKeyDerivation &plan);
	[[nodiscard]] static std::optional<QByteArray> Hmac(
		const QByteArray &key, const QByteArray &input);
	static void Clear(QByteArray &bytes);

  private:
	struct Keys;
	std::shared_ptr<Keys> _keys;

};

}
