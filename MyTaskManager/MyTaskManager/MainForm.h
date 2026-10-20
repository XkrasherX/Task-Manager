#pragma once
#include "ProcAPI.h"
#include <msclr/marshal_cppstd.h>

namespace MyTaskManager {

	using namespace System;
	using namespace System::ComponentModel;
	using namespace System::Collections;
	using namespace System::Windows::Forms;
	using namespace System::Data;
	using namespace System::Drawing;

	/// <summary>
	/// Summary for MainForm
	/// </summary>
	public ref class MainForm : public System::Windows::Forms::Form
	{
	public:
		MainForm(void)
		{
			InitializeComponent();
			mon = new ProcApi::Monitor();

			InitGrid();    
			InitGridThreads();
			UpdateProcs(); 
		}

	protected:
		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		~MainForm()
		{
			if (components)
			{
				delete components;
			}
			if (mon) {
				delete mon;
				mon = nullptr;
			}

		}
	private:
		// Функція для створення колонок таблиці програмно (щоб не клацати вручну)
		void InitGrid() {
			gridProcs->ColumnCount = 10;
			gridProcs->Columns[0]->Name = "PID";
			gridProcs->Columns[1]->Name = "Назва";
			gridProcs->Columns[2]->Name = "CPU %";
			gridProcs->Columns[3]->Name = "Клас пріоритету";
			gridProcs->Columns[4]->Name = "Стан";
			gridProcs->Columns[5]->Name = "WS (МБ)";
			gridProcs->Columns[6]->Name = "VA (МБ)";
			gridProcs->Columns[7]->Name = "Private (МБ)";
			gridProcs->Columns[8]->Name = "Потоки";
			gridProcs->Columns[9]->Name = "Власник";

			gridProcs->SelectionMode = DataGridViewSelectionMode::FullRowSelect; // Виділяти весь рядок
			gridProcs->AllowUserToAddRows = false; // Заборонити користувачу додавати рядки
			gridProcs->ReadOnly = true;            // Тільки для читання
			gridProcs->RowHeadersVisible = false;  // Сховати порожню ліву колонку
		}

		void InitGridThreads() {
			gridThreads->ColumnCount = 5;
			gridThreads->Columns[0]->Name = "TID";
			gridThreads->Columns[1]->Name = "PID";
			gridThreads->Columns[2]->Name = "CPU %";
			gridThreads->Columns[3]->Name = "Відносний пріоритет";
			gridThreads->Columns[4]->Name = "Стан";

			gridThreads->SelectionMode = DataGridViewSelectionMode::FullRowSelect;
			gridThreads->AllowUserToAddRows = false;
			gridThreads->ReadOnly = true;
			gridThreads->RowHeadersVisible = false;
		}

		// Функція, яка бере дані з нашого ядра і малює в таблиці
		void UpdateProcs() {
			unsigned long selectedPid = 0xFFFFFFFF;
			if (gridProcs->SelectedRows->Count > 0) {
				selectedPid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);
			}

			// Якщо увімкнено прапорець "Показати потоки", збираємо їх для обраного PID, інакше ні
			DWORD pidToQuery = (chkShowThreads->Checked && gridProcs->SelectedRows->Count > 0) ? selectedPid : 0xFFFFFFFF;
			mon->Refresh(pidToQuery);

			// Робимо копію звітів, щоб застосувати сортування та фільтри у вигляді відображення
			std::vector<ProcApi::ProcessInfo> procs = mon->Procs();

			// Застосовуємо сортування, якщо клікали на заголовок
			if (lastSortedCol != -1) {
				ProcApi::SortProcs(procs, lastSortedCol, sortAscending);
			}

			gridProcs->SuspendLayout();
			gridProcs->Rows->Clear();

			for (const auto& p : procs) {
				// Операційне визначення системних процесів за завданням:
				// Власник LocalSystem, LocalService, NetworkService або PID 0 і 4[cite: 2, 4]
				bool isSystem = p.isSystem || p.pid == 0 || p.pid == 4 ||
					p.owner.find(L"SYSTEM") != std::wstring::npos ||
					p.owner.find(L"LocalService") != std::wstring::npos ||
					p.owner.find(L"NetworkService") != std::wstring::npos;

				bool isSelected = (p.pid == selectedPid);

				// 1. «Лише обраний PID» має найвищий пріоритет над іншими фільтрами
				if (chkOnlySelected->Checked && !isSelected) {
					continue;
				}

				// 2. «Приховати системні» (але обраний PID ніколи не зникає)[cite: 4]
				if (chkHideSystem->Checked && isSystem && !isSelected) {
					continue;
				}

				array<String^>^ row = gcnew array<String^>(10);
				row[0] = p.pid.ToString();
				row[1] = gcnew String(p.name.c_str());
				row[2] = p.cpu.ToString("F1");
				row[3] = p.priorityKnown ? gcnew String(ProcApi::PriorityClassName(p.priorityClass).c_str()) : "—";
				row[4] = gcnew String(p.state.c_str());
				row[5] = p.memKnown ? (p.ws / (1024.0 * 1024.0)).ToString("F1") : "—";
				row[6] = p.vaMeasured ? (p.va / (1024.0 * 1024.0)).ToString("F1") : "не виміряно";
				row[7] = p.memKnown ? (p.priv / (1024.0 * 1024.0)).ToString("F1") : "—";
				row[8] = p.threads.ToString();
				row[9] = gcnew String(p.owner.c_str());

				int rowIndex = gridProcs->Rows->Add(row);

				// Зберігаємо виділення рядка після оновлення
				if (p.pid == selectedPid) {
					gridProcs->Rows[rowIndex]->Selected = true;
				}
			}
			gridProcs->ResumeLayout();

			lblStatus->Text = String::Format("Процесів: {0} | Логічних CPU: {1} | Доступ: {2}",
				procs.size(), mon->Logical(), ProcApi::IsElevated() ? "Адміністратор" : "Користувач");

			UpdateThreads();
			UpdateLog();
		}

		// Функція для малювання потоків обраного процесу
		void UpdateThreads() {
			const auto& threads = mon->Threads();
			gridThreads->SuspendLayout();
			gridThreads->Rows->Clear();

			for (const auto& t : threads) {
				array<String^>^ row = gcnew array<String^>(5);
				row[0] = t.tid.ToString();
				row[1] = t.pid.ToString();
				row[2] = t.cpu.ToString("F1");
				row[3] = t.priorityKnown ? gcnew String(ProcApi::ThreadPriorityName(t.relPriority).c_str()) : "—";
				row[4] = gcnew String(t.state.c_str());
				gridThreads->Rows->Add(row);
			}
			gridThreads->ResumeLayout();
		}

		// Функція для читання журналу подій (наприклад, повідомлень про завершення процесів)
		void UpdateLog() {
			std::vector<std::wstring> events = mon->TakeEvents();
			for (const auto& ev : events) {
				String^ s = gcnew String(ev.c_str());
				listLog->Items->Add(DateTime::Now.ToString("HH:mm:ss") + " | " + s);
				listLog->TopIndex = listLog->Items->Count - 1; // Прокрутка вниз
			}
		}

		// Функція для запису результатів дій у журнал
		void LogResult(const ProcApi::OpResult& r) {
			String^ msg = gcnew String(r.Format().c_str());
			listLog->Items->Add(DateTime::Now.ToString("HH:mm:ss") + " | " + msg);
			listLog->TopIndex = listLog->Items->Count - 1; // Прокручуємо в самий низ
		}

	private:
		ProcApi::Monitor* mon;
		bool sortAscending = true;
		int lastSortedCol = -1;
	private: System::Windows::Forms::Button^ btnRefresh;
	private: System::Windows::Forms::DataGridView^ gridProcs;
	private: System::Windows::Forms::StatusStrip^ statusStrip1;
	private: System::Windows::Forms::ToolStripStatusLabel^ lblStatus;
	private: System::Windows::Forms::Timer^ timerRefresh;
	private: System::Windows::Forms::DataGridView^ gridThreads;
	private: System::Windows::Forms::ListBox^ listLog;
private: System::Windows::Forms::ContextMenuStrip^ menuProcs;
private: System::Windows::Forms::ToolStripMenuItem^ menuTerminate;
private: System::Windows::Forms::ToolStripMenuItem^ menuSuspend;
private: System::Windows::Forms::ToolStripMenuItem^ menuResume;
private: System::Windows::Forms::Button^ btnLaunch;
private: System::Windows::Forms::ToolStripMenuItem^ пріоритетПроцесуToolStripMenuItem;
private: System::Windows::Forms::ToolStripMenuItem^ menuPriIdle;
private: System::Windows::Forms::ToolStripMenuItem^ menuPriBelow;
private: System::Windows::Forms::ToolStripMenuItem^ menuPriNormal;
private: System::Windows::Forms::ToolStripMenuItem^ menuPriAbove;
private: System::Windows::Forms::ToolStripMenuItem^ menuPriHigh;
private: System::Windows::Forms::CheckBox^ chkHideSystem;
private: System::Windows::Forms::CheckBox^ chkOnlySelected;
private: System::Windows::Forms::CheckBox^ chkShowThreads;
private: System::Windows::Forms::Button^ btnResetFilters;
private: System::Windows::Forms::ToolStripMenuItem^ menuAffinity;






	private: System::ComponentModel::IContainer^ components;


#pragma region Windows Form Designer generated code
		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		void InitializeComponent(void)
		{
			this->components = (gcnew System::ComponentModel::Container());
			this->btnRefresh = (gcnew System::Windows::Forms::Button());
			this->gridProcs = (gcnew System::Windows::Forms::DataGridView());
			this->menuProcs = (gcnew System::Windows::Forms::ContextMenuStrip(this->components));
			this->menuTerminate = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuSuspend = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuResume = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->пріоритетПроцесуToolStripMenuItem = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuPriIdle = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuPriBelow = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuPriNormal = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuPriAbove = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuPriHigh = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->menuAffinity = (gcnew System::Windows::Forms::ToolStripMenuItem());
			this->statusStrip1 = (gcnew System::Windows::Forms::StatusStrip());
			this->lblStatus = (gcnew System::Windows::Forms::ToolStripStatusLabel());
			this->timerRefresh = (gcnew System::Windows::Forms::Timer(this->components));
			this->gridThreads = (gcnew System::Windows::Forms::DataGridView());
			this->listLog = (gcnew System::Windows::Forms::ListBox());
			this->btnLaunch = (gcnew System::Windows::Forms::Button());
			this->chkHideSystem = (gcnew System::Windows::Forms::CheckBox());
			this->chkOnlySelected = (gcnew System::Windows::Forms::CheckBox());
			this->chkShowThreads = (gcnew System::Windows::Forms::CheckBox());
			this->btnResetFilters = (gcnew System::Windows::Forms::Button());
			(cli::safe_cast<System::ComponentModel::ISupportInitialize^>(this->gridProcs))->BeginInit();
			this->menuProcs->SuspendLayout();
			this->statusStrip1->SuspendLayout();
			(cli::safe_cast<System::ComponentModel::ISupportInitialize^>(this->gridThreads))->BeginInit();
			this->SuspendLayout();
			// 
			// btnRefresh
			// 
			this->btnRefresh->Location = System::Drawing::Point(12, 33);
			this->btnRefresh->Name = L"btnRefresh";
			this->btnRefresh->Size = System::Drawing::Size(127, 42);
			this->btnRefresh->TabIndex = 0;
			this->btnRefresh->Text = L"Оновити";
			this->btnRefresh->UseVisualStyleBackColor = true;
			this->btnRefresh->Click += gcnew System::EventHandler(this, &MainForm::btnRefresh_Click);
			// 
			// gridProcs
			// 
			this->gridProcs->ColumnHeadersHeightSizeMode = System::Windows::Forms::DataGridViewColumnHeadersHeightSizeMode::AutoSize;
			this->gridProcs->ContextMenuStrip = this->menuProcs;
			this->gridProcs->Location = System::Drawing::Point(12, 389);
			this->gridProcs->Name = L"gridProcs";
			this->gridProcs->RowHeadersWidth = 62;
			this->gridProcs->RowTemplate->Height = 28;
			this->gridProcs->Size = System::Drawing::Size(1694, 292);
			this->gridProcs->TabIndex = 1;
			this->gridProcs->ColumnHeaderMouseClick += gcnew System::Windows::Forms::DataGridViewCellMouseEventHandler(this, &MainForm::gridProcs_ColumnHeaderMouseClick);
			// 
			// menuProcs
			// 
			this->menuProcs->ImageScalingSize = System::Drawing::Size(24, 24);
			this->menuProcs->Items->AddRange(gcnew cli::array< System::Windows::Forms::ToolStripItem^  >(5) {
				this->menuTerminate, this->menuSuspend,
					this->menuResume, this->пріоритетПроцесуToolStripMenuItem, this->menuAffinity
			});
			this->menuProcs->Name = L"menuProcs";
			this->menuProcs->Size = System::Drawing::Size(249, 164);
			this->menuProcs->Opening += gcnew System::ComponentModel::CancelEventHandler(this, &MainForm::menuProcs_Opening);
			// 
			// menuTerminate
			// 
			this->menuTerminate->Name = L"menuTerminate";
			this->menuTerminate->Size = System::Drawing::Size(248, 32);
			this->menuTerminate->Text = L"Завершити процес";
			this->menuTerminate->Click += gcnew System::EventHandler(this, &MainForm::menuTerminate_Click);
			// 
			// menuSuspend
			// 
			this->menuSuspend->Name = L"menuSuspend";
			this->menuSuspend->Size = System::Drawing::Size(248, 32);
			this->menuSuspend->Text = L"Призупинити";
			this->menuSuspend->Click += gcnew System::EventHandler(this, &MainForm::menuSuspend_Click);
			// 
			// menuResume
			// 
			this->menuResume->Name = L"menuResume";
			this->menuResume->Size = System::Drawing::Size(248, 32);
			this->menuResume->Text = L"Відновити";
			this->menuResume->Click += gcnew System::EventHandler(this, &MainForm::menuResume_Click);
			// 
			// пріоритетПроцесуToolStripMenuItem
			// 
			this->пріоритетПроцесуToolStripMenuItem->DropDownItems->AddRange(gcnew cli::array< System::Windows::Forms::ToolStripItem^  >(5) {
				this->menuPriIdle,
					this->menuPriBelow, this->menuPriNormal, this->menuPriAbove, this->menuPriHigh
			});
			this->пріоритетПроцесуToolStripMenuItem->Name = L"пріоритетПроцесуToolStripMenuItem";
			this->пріоритетПроцесуToolStripMenuItem->Size = System::Drawing::Size(248, 32);
			this->пріоритетПроцесуToolStripMenuItem->Text = L"Пріоритет процесу";
			// 
			// menuPriIdle
			// 
			this->menuPriIdle->Name = L"menuPriIdle";
			this->menuPriIdle->Size = System::Drawing::Size(227, 34);
			this->menuPriIdle->Text = L"Idle";
			this->menuPriIdle->Click += gcnew System::EventHandler(this, &MainForm::menuPriIdle_Click);
			// 
			// menuPriBelow
			// 
			this->menuPriBelow->Name = L"menuPriBelow";
			this->menuPriBelow->Size = System::Drawing::Size(227, 34);
			this->menuPriBelow->Text = L"Below normal";
			this->menuPriBelow->Click += gcnew System::EventHandler(this, &MainForm::menuPriBelow_Click);
			// 
			// menuPriNormal
			// 
			this->menuPriNormal->Name = L"menuPriNormal";
			this->menuPriNormal->Size = System::Drawing::Size(227, 34);
			this->menuPriNormal->Text = L"Normal";
			this->menuPriNormal->Click += gcnew System::EventHandler(this, &MainForm::menuPriNormal_Click);
			// 
			// menuPriAbove
			// 
			this->menuPriAbove->Name = L"menuPriAbove";
			this->menuPriAbove->Size = System::Drawing::Size(227, 34);
			this->menuPriAbove->Text = L"Above normal";
			this->menuPriAbove->Click += gcnew System::EventHandler(this, &MainForm::menuPriAbove_Click);
			// 
			// menuPriHigh
			// 
			this->menuPriHigh->Name = L"menuPriHigh";
			this->menuPriHigh->Size = System::Drawing::Size(227, 34);
			this->menuPriHigh->Text = L"High";
			this->menuPriHigh->Click += gcnew System::EventHandler(this, &MainForm::menuPriHigh_Click);
			// 
			// menuAffinity
			// 
			this->menuAffinity->Name = L"menuAffinity";
			this->menuAffinity->Size = System::Drawing::Size(248, 32);
			this->menuAffinity->Text = L"Відповідність ядрам";
			this->menuAffinity->Click += gcnew System::EventHandler(this, &MainForm::menuAffinity_Click);
			// 
			// statusStrip1
			// 
			this->statusStrip1->ImageScalingSize = System::Drawing::Size(24, 24);
			this->statusStrip1->Items->AddRange(gcnew cli::array< System::Windows::Forms::ToolStripItem^  >(1) { this->lblStatus });
			this->statusStrip1->Location = System::Drawing::Point(0, 811);
			this->statusStrip1->Name = L"statusStrip1";
			this->statusStrip1->Size = System::Drawing::Size(1718, 32);
			this->statusStrip1->TabIndex = 2;
			this->statusStrip1->Text = L"statusStrip1";
			// 
			// lblStatus
			// 
			this->lblStatus->Name = L"lblStatus";
			this->lblStatus->Size = System::Drawing::Size(179, 25);
			this->lblStatus->Text = L"toolStripStatusLabel1";
			// 
			// timerRefresh
			// 
			this->timerRefresh->Enabled = true;
			this->timerRefresh->Interval = 15000;
			this->timerRefresh->Tick += gcnew System::EventHandler(this, &MainForm::timerRefresh_Tick);
			// 
			// gridThreads
			// 
			this->gridThreads->ColumnHeadersHeightSizeMode = System::Windows::Forms::DataGridViewColumnHeadersHeightSizeMode::AutoSize;
			this->gridThreads->Location = System::Drawing::Point(12, 96);
			this->gridThreads->Name = L"gridThreads";
			this->gridThreads->RowHeadersWidth = 62;
			this->gridThreads->RowTemplate->Height = 28;
			this->gridThreads->Size = System::Drawing::Size(1694, 287);
			this->gridThreads->TabIndex = 3;
			// 
			// listLog
			// 
			this->listLog->FormattingEnabled = true;
			this->listLog->ItemHeight = 20;
			this->listLog->Location = System::Drawing::Point(12, 689);
			this->listLog->Name = L"listLog";
			this->listLog->Size = System::Drawing::Size(1694, 104);
			this->listLog->TabIndex = 4;
			// 
			// btnLaunch
			// 
			this->btnLaunch->Location = System::Drawing::Point(145, 38);
			this->btnLaunch->Name = L"btnLaunch";
			this->btnLaunch->Size = System::Drawing::Size(138, 37);
			this->btnLaunch->TabIndex = 6;
			this->btnLaunch->Text = L"Запустити";
			this->btnLaunch->UseVisualStyleBackColor = true;
			this->btnLaunch->Click += gcnew System::EventHandler(this, &MainForm::btnLaunch_Click);
			// 
			// chkHideSystem
			// 
			this->chkHideSystem->AutoSize = true;
			this->chkHideSystem->Location = System::Drawing::Point(302, 44);
			this->chkHideSystem->Name = L"chkHideSystem";
			this->chkHideSystem->Size = System::Drawing::Size(187, 24);
			this->chkHideSystem->TabIndex = 8;
			this->chkHideSystem->Text = L"Приховати системні";
			this->chkHideSystem->UseVisualStyleBackColor = true;
			this->chkHideSystem->CheckedChanged += gcnew System::EventHandler(this, &MainForm::chkHideSystem_CheckedChanged);
			// 
			// chkOnlySelected
			// 
			this->chkOnlySelected->AutoSize = true;
			this->chkOnlySelected->Location = System::Drawing::Point(495, 45);
			this->chkOnlySelected->Name = L"chkOnlySelected";
			this->chkOnlySelected->Size = System::Drawing::Size(176, 24);
			this->chkOnlySelected->TabIndex = 10;
			this->chkOnlySelected->Text = L"Лише обраний PID";
			this->chkOnlySelected->UseVisualStyleBackColor = true;
			// 
			// chkShowThreads
			// 
			this->chkShowThreads->AutoSize = true;
			this->chkShowThreads->Location = System::Drawing::Point(677, 43);
			this->chkShowThreads->Name = L"chkShowThreads";
			this->chkShowThreads->Size = System::Drawing::Size(165, 24);
			this->chkShowThreads->TabIndex = 11;
			this->chkShowThreads->Text = L"Показати потоки";
			this->chkShowThreads->UseVisualStyleBackColor = true;
			// 
			// btnResetFilters
			// 
			this->btnResetFilters->Location = System::Drawing::Point(848, 30);
			this->btnResetFilters->Name = L"btnResetFilters";
			this->btnResetFilters->Size = System::Drawing::Size(144, 52);
			this->btnResetFilters->TabIndex = 12;
			this->btnResetFilters->Text = L"Скинути фільтри";
			this->btnResetFilters->UseVisualStyleBackColor = true;
			this->btnResetFilters->Click += gcnew System::EventHandler(this, &MainForm::btnResetFilters_Click);
			// 
			// MainForm
			// 
			this->AutoScaleDimensions = System::Drawing::SizeF(9, 20);
			this->AutoScaleMode = System::Windows::Forms::AutoScaleMode::Font;
			this->ClientSize = System::Drawing::Size(1718, 843);
			this->Controls->Add(this->btnResetFilters);
			this->Controls->Add(this->chkShowThreads);
			this->Controls->Add(this->chkOnlySelected);
			this->Controls->Add(this->chkHideSystem);
			this->Controls->Add(this->btnLaunch);
			this->Controls->Add(this->listLog);
			this->Controls->Add(this->gridThreads);
			this->Controls->Add(this->statusStrip1);
			this->Controls->Add(this->gridProcs);
			this->Controls->Add(this->btnRefresh);
			this->Name = L"MainForm";
			this->Text = L"MainForm";
			(cli::safe_cast<System::ComponentModel::ISupportInitialize^>(this->gridProcs))->EndInit();
			this->menuProcs->ResumeLayout(false);
			this->statusStrip1->ResumeLayout(false);
			this->statusStrip1->PerformLayout();
			(cli::safe_cast<System::ComponentModel::ISupportInitialize^>(this->gridThreads))->EndInit();
			this->ResumeLayout(false);
			this->PerformLayout();

		}
#pragma endregion

		private: System::Void gridProcs_ColumnHeaderMouseClick(System::Object^ sender, System::Windows::Forms::DataGridViewCellMouseEventArgs^ e) {
			if (lastSortedCol == e->ColumnIndex) {
				sortAscending = !sortAscending; // Змінюємо напрямок
			}
			else {
				sortAscending = true;
				lastSortedCol = e->ColumnIndex; // Нова колонка
			}
			UpdateProcs(); // Перемальовуємо таблицю з урахуванням сортування
		}

	private: System::Void btnRefresh_Click(System::Object^ sender, System::EventArgs^ e) {
		UpdateProcs();
	}
private: System::Void timerRefresh_Tick(System::Object^ sender, System::EventArgs^ e) {
	UpdateProcs();
}
private: System::Void menuProcs_Opening(System::Object^ sender, System::ComponentModel::CancelEventArgs^ e) {
}
private: System::Void menuTerminate_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	// Шукаємо цей процес у нашому ядрі
	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			String^ name = gcnew String(p.name.c_str());
			// Запитуємо підтвердження (як вимагає завдання)
			if (MessageBox::Show("Дійсно примусово завершити процес " + name + " (PID " + pid + ")?",
				"Підтвердження", MessageBoxButtons::YesNo, MessageBoxIcon::Warning) == System::Windows::Forms::DialogResult::Yes)
			{
				ProcApi::OpResult r = mon->Terminate(p);
				LogResult(r);
				UpdateProcs(); // Одразу оновлюємо таблицю
			}
			break;
		}
	}
}
private: System::Void menuSuspend_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->SuspendProcess(p);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void menuResume_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->ResumeProcess(p);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void btnLaunch_Click(System::Object^ sender, System::EventArgs^ e) {
	OpenFileDialog^ ofd = gcnew OpenFileDialog();
	ofd->Filter = "Executables (*.exe)|*.exe|All files (*.*)|*.*";

	if (ofd->ShowDialog() == System::Windows::Forms::DialogResult::OK) {
		std::wstring exePath = msclr::interop::marshal_as<std::wstring>(ofd->FileName);

		// Тут ви можете замість жорстких рядків підставити текст із ваших TextBox-полів на формі, 
		// наприклад: txtArgs->Text, txtDir->Text, chkSuspended->Checked
		std::wstring args = L"--threads 4 --dir images --out result --dark 50 --bright 200 --repeat 50";
		std::wstring workDir = L""; // Якщо порожньо, передасться nullptr
		bool createSuspended = false; // Змініть на true, якщо встановлено прапорець

		DWORD newPid = 0;
		// Викликаємо нативний метод Launch, який повністю виконує вимоги CreateProcessW
		ProcApi::OpResult r = mon->Launch(exePath, args, workDir, createSuspended, newPid);

		// Виводимо результат у журнал і оновлюємо таблицю
		LogResult(r);
		UpdateProcs();
	}
}
private: System::Void menuPriIdle_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->SetProcPriority(p, IDLE_PRIORITY_CLASS);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void menuPriHigh_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->SetProcPriority(p, HIGH_PRIORITY_CLASS);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void menuPriAbove_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->SetProcPriority(p, ABOVE_NORMAL_PRIORITY_CLASS);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void menuPriNormal_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->SetProcPriority(p, NORMAL_PRIORITY_CLASS);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void menuPriBelow_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	for (const auto& p : mon->Procs()) {
		if (p.pid == pid) {
			ProcApi::OpResult r = mon->SetProcPriority(p, BELOW_NORMAL_PRIORITY_CLASS);
			LogResult(r);
			UpdateProcs();
			break;
		}
	}
}
private: System::Void btnResetFilters_Click(System::Object^ sender, System::EventArgs^ e) {
	chkHideSystem->Checked = false;
	chkOnlySelected->Checked = false;
	chkShowThreads->Checked = true;
	lastSortedCol = -1; // Скидаємо сортування
	UpdateProcs();
}
private: System::Void chkHideSystem_CheckedChanged(System::Object^ sender, System::EventArgs^ e) {
}
private: System::Void menuAffinity_Click(System::Object^ sender, System::EventArgs^ e) {
	if (gridProcs->SelectedRows->Count == 0) return;
	unsigned long pid = Convert::ToUInt32(gridProcs->SelectedRows[0]->Cells[0]->Value);

	ProcApi::ProcessInfo targetProc;
	bool found = false;

	const auto& procs = mon->Procs();
	for (size_t i = 0; i < procs.size(); ++i) {
		if (procs[i].pid == pid) {
			targetProc = procs[i];
			found = true;
			break;
		}
	}
	if (!found) return;

	ULONGLONG procMask = 0, sysMask = 0;
	DWORD err = 0;
	if (!mon->GetProcAffinity(targetProc, procMask, sysMask, err)) {
		MessageBox::Show("Не вдалося отримати маску процесу, код помилки: " + err, "Помилка", MessageBoxButtons::OK, MessageBoxIcon::Error);
		return;
	}

	System::Windows::Forms::Form^ dlg = gcnew System::Windows::Forms::Form();
	dlg->Text = "Відповідність ядрам (CPU Affinity) - PID " + pid;
	dlg->Size = System::Drawing::Size(320, 240);
	dlg->StartPosition = System::Windows::Forms::FormStartPosition::CenterParent;
	dlg->FormBorderStyle = System::Windows::Forms::FormBorderStyle::FixedDialog;
	dlg->MaximizeBox = false;
	dlg->MinimizeBox = false;

	System::Windows::Forms::Label^ lblInfo = gcnew System::Windows::Forms::Label();
	lblInfo->Text = "Виберіть доступні ядра для процесу:";
	lblInfo->Location = System::Drawing::Point(15, 15);
	lblInfo->AutoSize = true;
	dlg->Controls->Add(lblInfo);

	System::Windows::Forms::FlowLayoutPanel^ panel = gcnew System::Windows::Forms::FlowLayoutPanel();
	panel->Location = System::Drawing::Point(15, 45);
	panel->Size = System::Drawing::Size(275, 110);
	panel->FlowDirection = System::Windows::Forms::FlowDirection::TopDown;
	panel->AutoScroll = true;
	dlg->Controls->Add(panel);

	DWORD logicalCores = mon->Logical();

	// ЗАМІНИЛИ std::vector НА КЕРОВАНИЙ МАСИВ .NET:
	array<System::Windows::Forms::CheckBox^>^ checkBoxes = gcnew array<System::Windows::Forms::CheckBox^>(logicalCores);

	for (DWORD i = 0; i < logicalCores; ++i) {
		checkBoxes[i] = gcnew System::Windows::Forms::CheckBox();
		checkBoxes[i]->Text = "CPU " + i;
		checkBoxes[i]->Checked = (procMask & (1ULL << i)) != 0;
		if ((sysMask & (1ULL << i)) == 0) {
			checkBoxes[i]->Enabled = false;
		}
		panel->Controls->Add(checkBoxes[i]);
	}

	System::Windows::Forms::Button^ btnOk = gcnew System::Windows::Forms::Button();
	btnOk->Text = "Застосувати";
	btnOk->DialogResult = System::Windows::Forms::DialogResult::OK;
	btnOk->Location = System::Drawing::Point(110, 165);
	dlg->Controls->Add(btnOk);
	dlg->AcceptButton = btnOk;

	if (dlg->ShowDialog() == System::Windows::Forms::DialogResult::OK) {
		ULONGLONG newMask = 0;
		for (DWORD i = 0; i < logicalCores; ++i) {
			if (checkBoxes[i]->Checked) {
				newMask |= (1ULL << i);
			}
		}

		ProcApi::OpResult r = mon->SetProcAffinity(targetProc, newMask);
		LogResult(r);
		UpdateProcs();
	}
}
};
}
