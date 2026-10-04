#include "NoisePrintDialog.h"
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>

NoisePrintDialog::NoisePrintDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Remove with Noise Print"));
    auto* layout = new QFormLayout(this);
    m_amount = new QDoubleSpinBox(this);
    m_amount->setRange(0, 40);
    m_amount->setValue(12);
    m_amount->setSuffix(tr(" dB"));
    m_floor = new QDoubleSpinBox(this);
    m_floor->setRange(-120, 0);
    m_floor->setValue(-20);
    m_floor->setSuffix(tr(" dB"));
    layout->addRow(tr("Amount:"), m_amount);
    layout->addRow(tr("Floor:"), m_floor);
    auto* buttons = new QDialogButtonBox(this);
    buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);
}
double NoisePrintDialog::amountDb() const { return m_amount->value(); }
double NoisePrintDialog::floorDb() const { return m_floor->value(); }
