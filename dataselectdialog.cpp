/**********************************************************************
Copyright 2014-2016 The RIVET Developers. See the COPYRIGHT file at
the top-level directory of this distribution.

This file is part of RIVET.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**********************************************************************/

#include "dataselectdialog.h"
#include "ui_dataselectdialog.h"

#include "api.h"
#include "interface/console_interaction.h"
#include "interface/file_input_reader.h"
#include "interface/input_parameters.h"
#include "interface/input_manager.h"

#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include <boost/algorithm/string.hpp>
#include <fstream>


DataSelectDialog::DataSelectDialog(InputParameters& params, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::DataSelectDialog)
    , params(params)
    , data_selected(false)
{
    ui->setupUi(this);
    //set initial values
    if (!params.fileName.empty()) {
        detect_file_type();
        ui->homDimSpinBox->setValue(params.hom_degree);
        ui->xbinSpinBox->setValue(params.x_bins);
        ui->ybinSpinBox->setValue(params.y_bins);
    } else {
        ui->homDimSpinBox->setValue(0);
        ui->xbinSpinBox->setValue(10);
        ui->ybinSpinBox->setValue(10);
    }
    ui->maxDistHelp->setText(QChar(0x221E));
    ui->maxDistHelp->setStyleSheet("QPushButton { font : 30px; qproperty-alignment: AlignTop; }");
}

DataSelectDialog::~DataSelectDialog()
{
    delete ui;
}

void DataSelectDialog::closeEvent(QCloseEvent* event)
{
    event->accept();

    if (!data_selected) {
        qobject_cast<QWidget*>(this->parent())->close();
        exit(0);
    }
}

void DataSelectDialog::showEvent(QShowEvent* event)
{
    event->accept();

    // reset the parameter frame when a new dialog is created

    ui->xAxisLabel->setText("");
    ui->yAxisLabel->setText("");
    ui->xRevCheckBox->setChecked(false);
    ui->yRevCheckBox->setChecked(false);
    ui->maxDistBox->setEnabled(true);
    ui->maxDistBox->setText("");
    ui->maxDistHelp->setToolTip("");
    ui->maxDistHelp->setEnabled(false);
    ui->dataTypeComboBox->setCurrentIndex(0);
    ui->dataTypeComboBox->setEnabled(true);
    ui->xbinSpinBox->setValue(10);
    ui->ybinSpinBox->setValue(10);
    ui->homDimSpinBox->setSpecialValueText("");
    ui->homDimSpinBox->setEnabled(true);
    ui->homDimSpinBox->setValue(0);
    ui->parameterFrame->setEnabled(false);
    ui->computeButton->setEnabled(false);
    ui->filterComboBox->setEnabled(true);
    ui->filterComboBox->setCurrentIndex(0);
}

void DataSelectDialog::on_computeButton_clicked()
{
    // read in the input parameters from the dialog

    params.hom_degree = ui->homDimSpinBox->value();
    params.x_bins = ui->xbinSpinBox->value();
    params.y_bins = ui->ybinSpinBox->value();
    params.x_label = ui->xAxisLabel->text().toStdString();
    params.y_label = ui->yAxisLabel->text().toStdString();
    params.md_string = ui->maxDistBox->text().toStdString();
    params.x_reverse = ui->xRevCheckBox->checkState();
    params.y_reverse = ui->yRevCheckBox->checkState();
    params.type = ui->dataTypeComboBox->currentText().toStdString();
    if (params.type != "bifiltration" && params.type != "firep" && params.type != "RIVET_msgpack")
        params.bifil = ui->filterComboBox->currentText().toStdString();

    data_selected = true;

    emit dataSelected();

    close();
}

void DataSelectDialog::on_openFileButton_clicked()
{
    const QString DEFAULT_DIR_KEY("default_load_dir");

    QSettings settings;

    //prompt user to select a file
    auto selected_file = QFileDialog::getOpenFileName(
        this, tr("Open Data File"), settings.value(DEFAULT_DIR_KEY).toString(), "");

    if (!selected_file.isNull()) {

        params.fileName = selected_file.toUtf8().constData();
        QDir current_dir;
        settings.setValue(DEFAULT_DIR_KEY, current_dir.absoluteFilePath(selected_file));
        detect_file_type();

    }
} //end on_openFileButton_clicked()

void DataSelectDialog::on_maxDistHelp_clicked()
{
    ui->maxDistBox->setText("inf");
}

void DataSelectDialog::detect_file_type()
{

    ui->homDimSpinBox->setSpecialValueText("");
    //this turns off the special value text (i.e. zero is displayed like normal)

    ui->homDimSpinBox->setEnabled(true);
    ui->homDimSpinBox->setValue(0);

    // reset the values and states of everything when a new file is selected
    params.x_label = "";
    params.y_label = "distance";

    params.x_reverse = false;
    params.y_reverse = false;

    params.x_bins = 10;
    params.y_bins = 10;

    params.type = "points";
    params.max_dist = -1;
    params.md_string = "inf";
    params.hom_degree = 0;

    ui->maxDistBox->setEnabled(true);
    ui->maxDistBox->setText("");
    ui->maxDistHelp->setToolTip("");
    ui->maxDistHelp->setEnabled(false);

    ui->dataTypeComboBox->setCurrentIndex(0);
    ui->dataTypeComboBox->setEnabled(true);

    ui->filterComboBox->setCurrentIndex(0);
    ui->filterComboBox->setEnabled(true);

    ui->xbinSpinBox->setValue(10);
    ui->ybinSpinBox->setValue(10);

    std::ifstream infile(params.fileName);

    if (!infile.is_open()) {
        invalid_file("Unable to read file.");
        return;
    }

    FileInputReader reader(infile);
    if (!reader.has_next_line()) {
        invalid_file("Empty file.");
        return;
    }

    // determine parameters specified in the input file
    InputManager inputManager(params);
    inputManager.start();

    // modify values in the parameter frame and dataselect dialog
    // by reading values in from the input file
    // depending on file type, some options in the parameter frame are unavailable

    QString type_string("This file appears to contain ");
    bool raw = true;
    
    ui->homDimSpinBox->setValue(params.hom_degree);
    ui->maxDistBox->setText(QString::fromStdString(params.md_string));

    if (params.type == "points") {
        type_string += "point-cloud data.";
        ui->dataTypeComboBox->setCurrentIndex(0);
    }
    else if (params.type == "points_fn") {
        type_string += "point-cloud data with function values.";
        ui->dataTypeComboBox->setCurrentIndex(1);
    }
    else if (params.type == "metric") {
        type_string += "metric data.";
        ui->dataTypeComboBox->setCurrentIndex(2);
    }
    else if (params.type == "metric_fn") {
        type_string += "metric data with function values.";
        ui->dataTypeComboBox->setCurrentIndex(3);
    }
    else if (params.type == "bifiltration") {
        ui->dataTypeComboBox->setCurrentIndex(4);
        ui->dataTypeComboBox->setEnabled(false);
        type_string += "bifiltration data.";
        ui->maxDistBox->setText("N/A");
        ui->maxDistBox->setEnabled(false);
        ui->filterComboBox->setEnabled(false);
    }
    else if (params.type == "firep") {
        ui->dataTypeComboBox->setCurrentIndex(5);
        ui->dataTypeComboBox->setEnabled(false);
        type_string += "free implicit representation data.";

        ui->homDimSpinBox->setSpecialValueText("N/A");
        //the spinbox will show the special value text when the value is the minimum value (i.e. zero)
        ui->homDimSpinBox->setValue(0);
        ui->homDimSpinBox->setEnabled(false);

        ui->maxDistBox->setText("N/A");
        ui->maxDistBox->setEnabled(false);
        ui->filterComboBox->setEnabled(false);
    }
    else if (params.type == "RIVET_msgpack") {
        ui->dataTypeComboBox->setCurrentIndex(6);
        ui->dataTypeComboBox->setEnabled(false);
        type_string += "pre-computed RIVET data.";
        raw = false;
    }

    ui->xAxisLabel->setText(QString::fromStdString(params.x_label));
    ui->yAxisLabel->setText(QString::fromStdString(params.y_label));

    if (params.bifil == "degree")
        ui->filterComboBox->setCurrentIndex(0);
    else if (params.bifil == "function")
        ui->filterComboBox->setCurrentIndex(1);

    if (params.x_reverse)
        ui->xRevCheckBox->setChecked(true);

    if (params.y_reverse)
        ui->yRevCheckBox->setChecked(true);

    if (params.x_bins > 0)
        ui->xbinSpinBox->setValue(params.x_bins);

    if (params.y_bins > 0)
        ui->xbinSpinBox->setValue(params.y_bins);

    ui->fileTypeLabel->setText(type_string);
    QFileInfo fileInfo(QString::fromStdString(params.fileName));
    ui->fileLabel->setText("Selected file: " + fileInfo.fileName());

    // need this for filename in visualization window
    params.shortName = fileInfo.fileName().toUtf8().constData();

    ui->parameterFrame->setEnabled(raw);

    if (ui->maxDistBox->isEnabled()) {
        ui->maxDistHelp->setToolTip("Set distance to infinity");
        ui->maxDistHelp->setEnabled(true);
    }

    ui->computeButton->setEnabled(true);
    //force black text because on Mac Qt autodefault buttons have white text when enabled,
    //so they still look like they're disabled or weird in some way.
    ui->computeButton->setStyleSheet("QPushButton { color: black; }");


} //end detect_file_type()

// TODO: Use this function to make a pop up error for invalid files
void DataSelectDialog::invalid_file(const QString& message)
{
    ui->fileLabel->setText("Please select a file.");
    ui->parameterFrame->setEnabled(false);
    ui->computeButton->setEnabled(false);
    ui->fileTypeLabel->setText(nullptr);
    QMessageBox errorBox(QMessageBox::Warning, "Error", message);
    errorBox.exec();
}
