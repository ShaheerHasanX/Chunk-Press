#include "chunkpress.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

namespace {

using namespace chunkpress;

const char* stylesheet = R"(
QWidget { color: #182a26; font-family: "DejaVu Sans"; font-size: 13px; }
QWidget#window { background: #f2f5f2; }
QFrame#banner { background: #16342e; border-radius: 12px; }
QLabel#brand { background: transparent; color: #ffffff; font-size: 27px; font-weight: 700; }
QLabel#subtitle { background: transparent; color: #bcd9cf; font-size: 12px; }
QFrame#panel { background: #ffffff; border: 1px solid #d9e3de; border-radius: 10px; }
QFrame#line { background: #e3ebe6; max-height: 1px; }
QLabel#heading { font-size: 16px; font-weight: 700; }
QLabel#field, QLabel#metricLabel { font-size: 11px; color: #4d645b; font-weight: 600; }
QLabel#muted { color: #50685e; font-size: 12px; }
QLabel#metric { font-size: 20px; font-weight: 700; }
QLabel#state { color: #075f4c; font-size: 13px; font-weight: 700; }
QLabel#error { color: #972f24; font-weight: 600; }
QLineEdit, QSpinBox, QComboBox {
    background: #fbfcfb; border: 1px solid #c9d7d0; border-radius: 6px; min-height: 31px; padding: 0 8px;
    selection-background-color: #096c57;
}
QComboBox { padding-right: 25px; }
QComboBox::drop-down { border: none; width: 25px; }
QComboBox::down-arrow, QSpinBox::down-arrow { image: url(:/icons/chevron-down.png); width: 9px; height: 6px; }
QSpinBox { padding-right: 24px; }
QSpinBox::up-button, QSpinBox::down-button { subcontrol-origin: border; width: 22px; border: 0; }
QSpinBox::up-button { subcontrol-position: top right; margin-top: 1px; }
QSpinBox::down-button { subcontrol-position: bottom right; margin-bottom: 1px; }
QSpinBox::up-arrow { image: url(:/icons/chevron-up.png); width: 9px; height: 6px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #e3eee7; border-radius: 4px; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPushButton:focus { border: 1px solid #187765; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { background: #f0f3f1; color: #5b6e66; }
QPushButton {
    border: 1px solid #c5d4cc; border-radius: 6px; background: #ffffff; min-height: 31px; padding: 0 12px;
    font-weight: 600;
}
QPushButton:hover { background: #e9f2ed; }
QPushButton:pressed { background: #d5e7df; }
QPushButton:disabled { background: #f0f3f1; color: #667c72; border-color: #e0e9e3; }
QPushButton#start { background: #076752; color: #ffffff; border-color: #076752; min-height: 37px; }
QPushButton#start:hover { background: #07533f; }
QPushButton#start:focus { border: 2px solid #173a30; }
QPushButton#start:disabled { background: #e3ece7; color: #557066; border-color: #e3ece7; }
QProgressBar { border: none; border-radius: 4px; background: #e7efea; max-height: 8px; }
QProgressBar::chunk { background: #168068; border-radius: 4px; }
QTableWidget { border: 1px solid #e2ebe5; border-radius: 5px; background: white; alternate-background-color: #f2f7f4; outline: none; }
QHeaderView::section { background: #e9f1ec; border: 0; color: #3a5549; padding: 6px 9px; font-weight: 600; }
QTableWidget::item { padding: 2px 9px; }
)";

class Mark : public QWidget {
public:
    Mark() { setFixedSize(46, 46); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#275047"));
        painter.drawRoundedRect(rect(), 9, 9);
        const QPen outline(QColor("#c3e6d4"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(12, 17, 22, 17), 2, 2);
        painter.drawRoundedRect(QRectF(10, 12, 26, 5), 2, 2);
        painter.drawLine(QPointF(20, 23), QPointF(26, 23));
    }
};

QLabel* label(const QString& text, const char* style = "") {
    auto* widget = new QLabel(text);
    widget->setObjectName(style);
    widget->setTextFormat(Qt::PlainText);
    return widget;
}

QFrame* divider() {
    auto* line = new QFrame;
    line->setObjectName("line");
    line->setFixedHeight(1);
    return line;
}

QString sizeText(std::uint64_t size) {
    constexpr double kib = 1024;
    if (size >= kib * kib * kib) return QString::number(size / (kib * kib * kib), 'f', 2) + " GiB";
    if (size >= kib * kib) return QString::number(size / (kib * kib), 'f', 2) + " MiB";
    if (size >= kib) return QString::number(size / kib, 'f', 1) + " KiB";
    return QString::number(static_cast<qulonglong>(size)) + " B";
}

Path pathOf(const QString& text) {
    return Path(QFile::encodeName(text).constData());
}

QString workerText(WorkerState worker) {
    switch (worker) {
    case WorkerState::idle: return "Idle";
    case WorkerState::compressing: return "Compressing";
    case WorkerState::waitingForWriter: return "Waiting for writer";
    case WorkerState::stopped: return "Stopped";
    }
    return {};
}

class Window : public QWidget {
public:
    Window() {
        setWindowTitle("Chunkpress");
        setObjectName("window");
        setMinimumSize(820, 625);
        resize(940, 650);

        auto* main = new QVBoxLayout(this);
        main->setContentsMargins(22, 20, 22, 20);
        main->setSpacing(15);
        main->addWidget(makeBanner());
        auto* body = new QHBoxLayout;
        body->setSpacing(15);
        body->addWidget(makeForm(), 1);
        body->addWidget(makeMonitor(), 1);
        main->addLayout(body, 1);

        connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateMode(); });
        connect(workers_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            const bool fullPool = limit_->value() == limit_->maximum();
            limit_->setMaximum(value);
            if (fullPool) limit_->setValue(value);
            updateCapacityLimit();
        });
        connect(chunk_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { updateCapacityLimit(); });
        connect(capacity_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            if (control_) control_->resize(value);
        });
        connect(start_, &QPushButton::clicked, this, [this] { start(); });
        connect(pause_, &QPushButton::clicked, this, [this] {
            if (!control_) return;
            auto phase = control_->progress().phase;
            if (phase == Phase::paused || phase == Phase::pausing) control_->resume();
            else control_->pause();
            refresh();
        });
        connect(cancel_, &QPushButton::clicked, this, [this] {
            if (control_) control_->cancel();
            refresh();
        });
        connect(next_, &QPushButton::clicked, this, [this] {
            const auto archive = output_->text();
            const auto original = input_->text();
            mode_->setCurrentIndex(1);
            input_->setText(archive);
            suggestDestination();
            reference_->setText(original);
            start_->setFocus();
        });
        connect(input_, &QLineEdit::textEdited, this, [this] { suggestDestination(); });
        connect(output_, &QLineEdit::textEdited, this, [this](const QString& value) { customDestination_ = !value.isEmpty(); });
        connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
        timer_.setInterval(60);
        updateMode();
    }

    ~Window() override {
        if (control_) control_->cancel();
        if (job_.joinable()) job_.join();
    }

    bool busy() const { return bool(control_); }
    bool successful() const { return successful_; }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (control_) {
            closing_ = true;
            control_->cancel();
            event->ignore();
            refresh();
        } else {
            event->accept();
        }
    }

private:
    QFrame* makeBanner() {
        auto* banner = new QFrame;
        banner->setObjectName("banner");
        auto* layout = new QHBoxLayout(banner);
        layout->setContentsMargins(18, 13, 18, 13);
        layout->setSpacing(12);
        layout->addWidget(new Mark);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(2);
        copy->addWidget(label("Chunkpress", "brand"));
        copy->addWidget(label("Parallel file compression for Linux", "subtitle"));
        layout->addLayout(copy);
        layout->addStretch();
        return banner;
    }

    QWidget* fileRow(QLineEdit*& field, const char* name, const QString& title, QPushButton*& browse) {
        auto* row = new QWidget;
        auto* column = new QVBoxLayout(row);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        auto* caption = label(title, "field");
        column->addWidget(caption);
        auto* line = new QHBoxLayout;
        line->setSpacing(6);
        field = new QLineEdit;
        field->setObjectName(name);
        field->setAccessibleName(title);
        field->setClearButtonEnabled(true);
        field->setMinimumWidth(140);
        connect(field, &QLineEdit::textChanged, field, &QLineEdit::setToolTip);
        caption->setBuddy(field);
        line->addWidget(field, 1);
        browse = new QPushButton("Browse…");
        browse->setAccessibleName("Browse " + title);
        line->addWidget(browse);
        column->addLayout(line);
        return row;
    }

    QFrame* makeForm() {
        auto* frame = new QFrame;
        frame->setObjectName("panel");
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(16, 15, 16, 15);
        layout->setSpacing(8);
        layout->addWidget(label("File operation", "heading"));

        mode_ = new QComboBox;
        mode_->setObjectName("mode");
        mode_->setAccessibleName("File operation");
        mode_->addItems({"Compress", "Extract", "Verify"});
        layout->addWidget(mode_);

        auto* inputRow = fileRow(input_, "input", "Input file", browseInput_);
        inputCaption_ = inputRow->findChild<QLabel*>("field");
        layout->addWidget(inputRow);
        outputRow_ = fileRow(output_, "output", "Save as", browseOutput_);
        layout->addWidget(outputRow_);
        referenceRow_ = fileRow(reference_, "reference", "Original for byte comparison (optional)", browseReference_);
        layout->addWidget(referenceRow_);
        connect(browseInput_, &QPushButton::clicked, this, [this] {
            const QString filter = mode_->currentIndex() == 0 ? "All files (*)" : "Chunkpress archives (*.cpa);;All files (*)";
            auto file = QFileDialog::getOpenFileName(this, "Choose input file", input_->text(), filter);
            if (file.isEmpty()) return;
            input_->setText(file);
            suggestDestination();
        });
        connect(browseOutput_, &QPushButton::clicked, this, [this] {
            const auto file = QFileDialog::getSaveFileName(this, "Choose an output filename", output_->text(),
                                                          {}, nullptr, QFileDialog::DontConfirmOverwrite);
            if (file.isEmpty()) return;
            output_->setText(file);
            customDestination_ = true;
        });
        connect(browseReference_, &QPushButton::clicked, this, [this] {
            const auto file = QFileDialog::getOpenFileName(this, "Choose the original file", reference_->text());
            if (!file.isEmpty()) reference_->setText(file);
        });

        settings_ = new QWidget;
        auto* settings = new QVBoxLayout(settings_);
        settings->setContentsMargins(0, 3, 0, 0);
        settings->setSpacing(8);
        settings->addWidget(divider());
        settings->addWidget(label("Compression settings", "heading"));
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(4);
        const auto put = [grid](const QString& name, QWidget* field, int row, int column) {
            auto* caption = label(name, "field");
            field->setAccessibleName(name);
            caption->setBuddy(field);
            grid->addWidget(caption, row, column);
            grid->addWidget(field, row + 1, column);
        };

        workers_ = new QSpinBox;
        workers_->setObjectName("workers");
        workers_->setRange(1, maxWorkers);
        workers_->setValue(4);
        limit_ = new QSpinBox;
        limit_->setRange(1, 4);
        limit_->setValue(4);
        limit_->setToolTip("Maximum workers allowed to compress simultaneously.");
        chunk_ = new QSpinBox;
        chunk_->setRange(minChunk / 1024, maxChunk / 1024);
        chunk_->setValue(1024);
        chunk_->setSingleStep(64);
        capacity_ = new QSpinBox;
        capacity_->setObjectName("capacity");
        capacity_->setRange(1, maxBuffer);
        capacity_->setValue(8);
        capacity_->setKeyboardTracking(false);
        capacity_->setToolTip("Slots per queue. You can change this while compression runs or is paused.");
        priority_ = new QComboBox;
        priority_->addItems({"FIFO", "Even chunks first"});
        priority_->setToolTip("Even-numbered chunk IDs go first when queued. Equal priorities stay in arrival order.");
        put("Workers", workers_, 0, 0);
        put("Active worker limit", limit_, 0, 1);
        put("Chunk size (KiB)", chunk_, 2, 0);
        put("Slots per queue", capacity_, 2, 1);
        put("Queue priority", priority_, 4, 0);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        settings->addLayout(grid);
        settings->addWidget(label("Queue slots can be changed during compression.", "muted"));
        layout->addWidget(settings_);
        layout->addStretch();

        notice_ = label("", "error");
        notice_->setWordWrap(true);
        notice_->hide();
        layout->addWidget(notice_);
        start_ = new QPushButton("Compress file");
        start_->setObjectName("start");
        layout->addWidget(start_);
        return frame;
    }

    QFrame* makeMonitor() {
        auto* frame = new QFrame;
        frame->setObjectName("panel");
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(16, 15, 16, 15);
        layout->setSpacing(10);
        auto* header = new QHBoxLayout;
        header->addWidget(label("Job monitor", "heading"));
        header->addStretch();
        state_ = label("Ready", "state");
        header->addWidget(state_);
        layout->addLayout(header);
        details_ = label("Choose a file to start.", "muted");
        details_->setWordWrap(true);
        layout->addWidget(details_);
        progress_ = new QProgressBar;
        progress_->setAccessibleName("File progress");
        progress_->setRange(0, 1000);
        progress_->setTextVisible(false);
        layout->addWidget(progress_);

        auto* metrics = new QGridLayout;
        metrics->setHorizontalSpacing(9);
        metrics->setVerticalSpacing(1);
        const auto metric = [metrics](const QString& title, int column) {
            auto* value = label("—", "metric");
            metrics->addWidget(label(title, "metricLabel"), 0, column);
            metrics->addWidget(value, 1, column);
            metrics->setColumnStretch(column, 1);
            return value;
        };
        originalMetric_ = metric("Original size", 0);
        archiveMetric_ = metric("Archive size", 1);
        ratioMetric_ = metric("Archive / original", 2);
        layout->addLayout(metrics);
        layout->addWidget(divider());
        layout->addWidget(label("Worker activity", "heading"));
        queues_ = label("Queue activity appears during compression.", "muted");
        queues_->setWordWrap(true);
        layout->addWidget(queues_);

        activity_ = new QTableWidget(0, 2);
        activity_->setHorizontalHeaderLabels({"Worker", "State"});
        activity_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        activity_->verticalHeader()->hide();
        activity_->verticalHeader()->setDefaultSectionSize(26);
        activity_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        activity_->setSelectionMode(QAbstractItemView::NoSelection);
        activity_->setFocusPolicy(Qt::NoFocus);
        activity_->setAlternatingRowColors(true);
        activity_->setShowGrid(false);
        activity_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        activity_->setMinimumHeight(110);
        layout->addWidget(activity_, 1);

        auto* controls = new QHBoxLayout;
        pause_ = new QPushButton("Pause");
        cancel_ = new QPushButton("Cancel");
        next_ = new QPushButton("Extract archive");
        next_->setObjectName("next");
        next_->hide();
        controls->addWidget(pause_);
        controls->addWidget(cancel_);
        controls->addStretch();
        controls->addWidget(next_);
        layout->addLayout(controls);
        return frame;
    }

    void updateMode() {
        const auto operation = mode_->currentIndex();
        const auto inputLabel = operation == 0 ? "Input file" : "Archive file";
        inputCaption_->setText(inputLabel);
        input_->setAccessibleName(inputLabel);
        browseInput_->setAccessibleName(QString("Browse ") + inputLabel);
        settings_->setVisible(operation == 0);
        outputRow_->setVisible(operation != 2);
        referenceRow_->setVisible(operation != 0);
        start_->setText(QStringList{"Compress file", "Extract file", "Verify archive"}[operation]);
        input_->clear();
        output_->clear();
        reference_->clear();
        customDestination_ = false;
        resetMonitor();
        updateCapacityLimit();
    }

    void resetMonitor() {
        next_->hide();
        notice_->hide();
        state_->setText("Ready");
        details_->setText("Choose a file to start.");
        originalMetric_->setText("—");
        archiveMetric_->setText("—");
        ratioMetric_->setText("—");
        progress_->setValue(0);
        activity_->setRowCount(0);
        queues_->setText("Queue activity appears during compression.");
        queues_->setToolTip({});
        pause_->setText("Pause");
        pause_->setEnabled(false);
        cancel_->setEnabled(false);
    }

    void suggestDestination() {
        if (customDestination_ || mode_->currentIndex() == 2) return;
        const auto input = input_->text();
        if (input.isEmpty() || input.endsWith('/')) {
            output_->clear();
            return;
        }
        const QFileInfo file(input);
        auto filename = file.fileName();
        if (mode_->currentIndex() == 0) {
            output_->setText(input + ".cpa");
        } else {
            if (filename.endsWith(".cpa", Qt::CaseInsensitive)) filename.chop(4);
            if (filename.isEmpty()) filename = "file";
            output_->setText(file.dir().filePath("restored-" + filename));
        }
    }

    void updateCapacityLimit() {
        const int limit = (512 * 1024) / (2 * chunk_->value()) - workers_->value() - 3;
        const bool valid = limit >= 1;
        capacity_->setMaximum(std::clamp(limit, 1, static_cast<int>(maxBuffer)));
        if (!busy()) {
            const bool acceptable = mode_->currentIndex() != 0 || valid;
            start_->setEnabled(acceptable);
            if (!acceptable) showNotice("These settings exceed the chunk-buffer budget. Reduce workers or chunk size.");
            else notice_->hide();
        }
    }

    void showNotice(const QString& message) {
        notice_->setText(message);
        notice_->show();
    }

    void lockForm(bool locked) {
        for (QWidget* widget : std::initializer_list<QWidget*>{
                mode_, input_, output_, reference_, browseInput_, browseOutput_, browseReference_,
                workers_, limit_, chunk_, priority_, capacity_, start_}) {
            widget->setEnabled(!locked);
        }
    }

    void start() {
        if (control_) return;
        successful_ = false;
        resetMonitor();
        const int operation = mode_->currentIndex();
        const Path input = pathOf(input_->text());
        const Path output = pathOf(output_->text());
        const Path reference = pathOf(reference_->text());
        if (input.empty() || (operation != 2 && output.empty())) {
            showNotice(operation == 2 ? "Choose an archive to verify." : "Choose an input file and an output filename.");
            return;
        }

        Options options;
        options.workers = workers_->value();
        options.activeLimit = limit_->value();
        options.chunkSize = chunk_->value() * 1024;
        options.bufferCapacity = capacity_->value();
        options.priority = priority_->currentIndex() == 0 ? Priority::fifo : Priority::evenFirst;

        control_ = std::make_unique<Control>();
        completed_.store(false);
        lockForm(true);
        state_->setText("Preparing");
        details_->setText("Opening files and checking settings…");
        const auto reportError = [this](const QString& message) {
            control_.reset();
            lockForm(false);
            state_->setText("Failed");
            details_->setText("The operation could not start.");
            showNotice(message);
        };
        try {
            job_ = std::thread([this, operation, input, output, reference, options] {
                try {
                    if (operation == 0) result_ = compress(input, output, options, *control_);
                    else if (operation == 1) result_ = extract(input, output, reference, *control_);
                    else result_ = verify(input, reference, *control_);
                    error_.clear();
                } catch (const std::exception& error) {
                    error_ = QString::fromUtf8(error.what());
                }
                completed_.store(true, std::memory_order_release);
            });
        } catch (const std::exception& error) {
            reportError(QString::fromUtf8(error.what()));
            return;
        }
        timer_.start();
        refresh();
    }

    void showProgress(const Progress& data) {
        switch (data.phase) {
        case Phase::ready: state_->setText("Preparing"); break;
        case Phase::running: state_->setText("Running"); break;
        case Phase::pausing: state_->setText("Pausing"); break;
        case Phase::paused: state_->setText("Paused"); break;
        case Phase::cancelling: state_->setText("Stopping"); break;
        case Phase::finalizing: state_->setText("Finalizing"); break;
        case Phase::finished: break;
        }

        if (data.phase != Phase::ready) {
            const auto fraction = data.originalBytes == 0 ? 0.0 :
                static_cast<double>(data.processedBytes) / data.originalBytes;
            progress_->setValue(static_cast<int>(fraction * 1000));
            details_->setText(QString("%1%  ·  %2 of %3 chunks")
                                  .arg(QString::number(100 * fraction, 'f', 1))
                                  .arg(static_cast<qulonglong>(data.completedChunks))
                                  .arg(static_cast<qulonglong>(data.totalChunks)));
            showMetrics(data.originalBytes, data.archiveBytes);
        }
        const bool pausing = data.phase == Phase::paused || data.phase == Phase::pausing;
        const bool changing = data.phase == Phase::running || pausing;
        pause_->setEnabled(changing);
        pause_->setText(pausing ? "Resume" : "Pause");
        cancel_->setEnabled(data.phase == Phase::ready || changing);
        capacity_->setEnabled(changing && data.bufferCapacity > 0);

        if (data.bufferCapacity > 0) {
            const auto active = std::count(data.workers, data.workers + data.workerCount, WorkerState::compressing);
            queues_->setText(QString("Input %1/%2   ·   Output %3/%2   ·   %4 compressing")
                                 .arg(data.inputDepth).arg(data.bufferCapacity).arg(data.outputDepth).arg(active));
            queues_->setToolTip(QString("%1 readers active. Writer %2.").arg(data.readersActive)
                                   .arg(data.writerActive ? "active" : "idle"));
        } else if (data.phase != Phase::ready) {
            queues_->setText("Archive chunks are decoded in source order.");
            queues_->setToolTip({});
        }

        if (activity_->rowCount() != static_cast<int>(data.workerCount)) {
            activity_->setRowCount(data.workerCount);
            for (unsigned i = 0; i < data.workerCount; ++i) {
                activity_->setItem(i, 0, new QTableWidgetItem(QString("Worker %1").arg(i + 1)));
                activity_->setItem(i, 1, new QTableWidgetItem);
            }
        }
        for (unsigned i = 0; i < data.workerCount; ++i) {
            auto* item = activity_->item(i, 1);
            const auto text = workerText(data.workers[i]);
            if (item->text() != text) {
                item->setText(text);
                item->setForeground(QColor(data.workers[i] == WorkerState::compressing ? "#076752" : "#496057"));
            }
        }
    }

    void showMetrics(std::uint64_t original, std::uint64_t archive) {
        originalMetric_->setText(sizeText(original));
        archiveMetric_->setText(sizeText(archive));
        ratioMetric_->setText(original ? QString::number(100.0 * archive / original, 'f', 1) + "%" : "—");
    }

    void refresh() {
        if (!control_) return;
        showProgress(control_->progress());
        if (!completed_.load(std::memory_order_acquire)) return;

        job_.join();
        timer_.stop();
        showProgress(control_->progress());
        control_.reset();
        lockForm(false);
        pause_->setEnabled(false);
        cancel_->setEnabled(false);
        const int operation = mode_->currentIndex();
        const bool compare = !reference_->text().isEmpty();
        successful_ = error_.isEmpty();
        if (!successful_) {
            const bool cancelled = error_ == "Operation cancelled";
            state_->setText(cancelled ? "Cancelled" : "Failed");
            details_->setText(cancelled ? "The job stopped before completion." : "The operation did not complete.");
            if (!cancelled) showNotice(error_);
        } else {
            state_->setText("Complete");
            progress_->setValue(1000);
            showMetrics(result_.originalBytes, result_.archiveBytes);
            const auto message = operation == 0 ? "Archive created" : compare ?
                "Matched the original byte-for-byte" : operation == 1 ? "File restored and checksums verified" :
                "Archive integrity verified";
            details_->setText(QString("%1 in %2 s.").arg(message).arg(result_.seconds, 0, 'f', 3));
            next_->setVisible(operation == 0 && output_->text() != input_->text());
        }
        if (closing_) QTimer::singleShot(0, this, &QWidget::close);
    }

    QComboBox *mode_ = nullptr, *priority_ = nullptr;
    QLineEdit *input_ = nullptr, *output_ = nullptr, *reference_ = nullptr;
    QWidget *outputRow_ = nullptr, *referenceRow_ = nullptr, *settings_ = nullptr;
    QSpinBox *workers_ = nullptr, *limit_ = nullptr, *chunk_ = nullptr, *capacity_ = nullptr;
    QPushButton *browseInput_ = nullptr, *browseOutput_ = nullptr, *browseReference_ = nullptr;
    QPushButton *start_ = nullptr, *pause_ = nullptr, *cancel_ = nullptr, *next_ = nullptr;
    QLabel *inputCaption_ = nullptr;
    QLabel *notice_ = nullptr, *state_ = nullptr, *details_ = nullptr, *queues_ = nullptr;
    QLabel *originalMetric_ = nullptr, *archiveMetric_ = nullptr, *ratioMetric_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTableWidget* activity_ = nullptr;
    QTimer timer_;
    bool customDestination_ = false;
    bool closing_ = false;
    bool successful_ = false;
    std::unique_ptr<Control> control_;
    std::thread job_;
    std::atomic<bool> completed_{false};
    Result result_{};
    QString error_;
};

int smokeTest(QApplication& app, Window& window, const QString& screenshot) {
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const auto sourcePath = directory.filePath("batch records.log");
    const auto archive = sourcePath + ".cpa";
    const auto restored = directory.filePath("restored-batch records.log");
    const QByteArray sample = QByteArray("record_id=1742, event=accepted, group=09\n").repeated(105'000) + "tail";
    QFile source(sourcePath);
    if (!source.open(QIODevice::WriteOnly) || source.write(sample) != sample.size()) return 1;
    source.close();
    auto* input = window.findChild<QLineEdit*>("input");
    auto* output = window.findChild<QLineEdit*>("output");
    auto* reference = window.findChild<QLineEdit*>("reference");
    auto* mode = window.findChild<QComboBox*>("mode");
    auto* next = window.findChild<QPushButton*>("next");
    auto* start = window.findChild<QPushButton*>("start");
    input->setText(sourcePath);
    output->setText(archive);
    int stage = 0;
    QTimer steps;
    QObject::connect(&steps, &QTimer::timeout, &window, [&] {
        if (window.busy()) return;
        if (!window.successful()) {
            app.exit(2);
            return;
        }
        if (stage == 0) {
            if (!screenshot.isEmpty() && !window.grab().save(screenshot)) {
                app.exit(3);
                return;
            }
            ++stage;
            next->click();
            if (mode->currentIndex() != 1 || input->text() != archive || output->text() != restored || reference->text() != sourcePath) {
                app.exit(4);
                return;
            }
            start->click();
        } else if (stage == 1) {
            QFile copy(restored);
            if (!copy.open(QIODevice::ReadOnly) || copy.readAll() != sample) {
                app.exit(5);
                return;
            }
            ++stage;
            mode->setCurrentIndex(2);
            input->setText(archive);
            reference->setText(sourcePath);
            start->click();
        } else {
            app.exit(0);
        }
    });
    QTimer::singleShot(20'000, &app, [&] { app.exit(6); });
    start->click();
    steps.start(20);
    return app.exec();
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Chunkpress");
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setStyleSheet(QString::fromLatin1(stylesheet));
    Window window;
    window.show();
    const auto arguments = app.arguments();
    if (arguments.value(1) == "--smoke-test") return smokeTest(app, window, arguments.value(2));
    return app.exec();
}
